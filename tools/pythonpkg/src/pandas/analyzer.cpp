#include "duckdb_python/pyrelation.hpp"
#include "duckdb_python/pyconnection/pyconnection.hpp"
#include "duckdb_python/pyresult.hpp"
#include "duckdb_python/pandas/pandas_analyzer.hpp"
#include "duckdb_python/python_conversion.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

static bool SameTypeRealm(const LogicalType &a, const LogicalType &b) {
	auto a_id = a.id();
	auto b_id = b.id();
	if (a_id == b_id) {
		return true;
	}
	if (a_id > b_id) {
		return SameTypeRealm(b, a);
	}
	D_ASSERT(a_id < b_id);

	// anything ANY and under can transform to anything
	if (a_id <= LogicalTypeId::ANY) {
		return true;
	}

	auto a_is_nested = a.IsNested();
	auto b_is_nested = b.IsNested();
	// Both a and b are not nested
	if (!a_is_nested && !b_is_nested) {
		return true;
	}
	// Non-nested -> Nested is not possible
	if (!a_is_nested || !b_is_nested) {
		return false;
	}

	// From this point on, left and right are both nested
	D_ASSERT(a_id != b_id);
	// STRUCT -> LIST is not possible
	if (b_id == LogicalTypeId::LIST || a_id == LogicalTypeId::LIST) {
		return false;
	}
	return true;
}

static bool UpgradeType(LogicalType &left, const LogicalType &right);

static bool CheckTypeCompatibility(const LogicalType &left, const LogicalType &right) {
	if (!SameTypeRealm(left, right)) {
		return false;
	}
	if (!left.IsNested() || !right.IsNested()) {
		return true;
	}

	// Nested type IDs between left and right have to match
	if (left.id() != right.id()) {
		return false;
	}
	return true;
}

static bool IsStructColumnValid(const LogicalType &left, const LogicalType &right) {
	D_ASSERT(left.id() == LogicalTypeId::STRUCT && left.id() == right.id());

	//! Child types of the two structs
	auto &left_children = StructType::GetChildTypes(left);
	auto &right_children = StructType::GetChildTypes(right);

	if (left_children.size() != right_children.size()) {
		return false;
	}
	//! Compare keys of struct case-insensitively
	auto compare = CaseInsensitiveStringEquality();
	for (idx_t i = 0; i < left_children.size(); i++) {
		auto &left_child = left_children[i];
		auto &right_child = right_children[i];

		// keys in left and right don't match
		if (!compare(left_child.first, right_child.first)) {
			return false;
		}
		// Types are not compatible with each other
		if (!CheckTypeCompatibility(left_child.second, right_child.second)) {
			return false;
		}
	}
	return true;
}

static bool CombineStructTypes(LogicalType &result, const LogicalType &input) {
	D_ASSERT(input.id() == LogicalTypeId::STRUCT);
	auto &children = StructType::GetChildTypes(input);
	for (auto &type : children) {
		if (!UpgradeType(result, type.second)) {
			return false;
		}
	}
	return true;
}

static bool SatisfiesMapConstraints(const LogicalType &left, const LogicalType &right, LogicalType &map_value_type) {
	D_ASSERT(left.id() == LogicalTypeId::STRUCT && left.id() == right.id());

	if (!CombineStructTypes(map_value_type, left)) {
		return false;
	}
	if (!CombineStructTypes(map_value_type, right)) {
		return false;
	}
	return true;
}

static LogicalType ConvertStructToMap(LogicalType &map_value_type) {
	// TODO: find a way to figure out actual type of the keys, not just the converted one
	return LogicalType::MAP(LogicalType::VARCHAR, map_value_type);
}

// This is similar to ForceMaxLogicalType but we have custom rules around combining STRUCT types
// And because of that we have to avoid ForceMaxLogicalType for every nested type
static bool UpgradeType(LogicalType &left, const LogicalType &right) {
	if (left.id() == LogicalTypeId::SQLNULL) {
		// Early out for upgrading null
		left = right;
		return true;
	}

	if (left.IsNested() && right.id() == LogicalTypeId::SQLNULL) {
		return true;
	}

	switch (left.id()) {
	case LogicalTypeId::LIST: {
		if (right.id() != left.id()) {
			// Not both sides are LIST, not compatible
			// FIXME: maybe compatible with ARRAY type??
			return false;
		}
		LogicalType child_type = LogicalType::SQLNULL;
		if (!UpgradeType(child_type, ListType::GetChildType(left))) {
			return false;
		}
		if (!UpgradeType(child_type, ListType::GetChildType(right))) {
			return false;
		}
		left = LogicalType::LIST(child_type);
		return true;
	}
	case LogicalTypeId::ARRAY: {
		throw InternalException("ARRAY types are not being detected yet, this should never happen");
	}
	case LogicalTypeId::STRUCT: {
		if (right.id() == LogicalTypeId::STRUCT) {
			bool valid_struct = IsStructColumnValid(left, right);
			if (valid_struct) {
				child_list_t<LogicalType> children;
				auto child_count = StructType::GetChildCount(right);
				D_ASSERT(child_count == StructType::GetChildCount(left));
				// Combine all types from left and right
				for (idx_t i = 0; i < child_count; i++) {
					auto &right_child = StructType::GetChildType(right, i);
					auto new_child = StructType::GetChildType(left, i);

					auto child_name = StructType::GetChildName(left, i);
					if (!UpgradeType(new_child, right_child)) {
						return false;
					}
					children.push_back(std::make_pair(child_name, new_child));
				}
				left = LogicalType::STRUCT(std::move(children));
			} else {
				LogicalType value_type = LogicalType::SQLNULL;
				if (SatisfiesMapConstraints(left, right, value_type)) {
					// Combine all the child types together, becoming the value_type for the resulting MAP
					left = ConvertStructToMap(value_type);
				} else {
					return false;
				}
			}
		} else if (right.id() == LogicalTypeId::MAP) {
			// Left: STRUCT, Right: MAP
			// Combine all the child types of the STRUCT into the value type of the MAP
			auto value_type = MapType::ValueType(right);
			if (!CombineStructTypes(value_type, left)) {
				return false;
			}
			left = LogicalType::MAP(LogicalType::VARCHAR, value_type);
		} else {
			return false;
		}
		return true;
	}
	case LogicalTypeId::UNION: {
		throw InternalException("UNION types are not being detected yet, this should never happen");
	}
	case LogicalTypeId::MAP: {
		if (right.id() == LogicalTypeId::MAP) {
			// Key Type
			LogicalType key_type = LogicalType::SQLNULL;
			if (!UpgradeType(key_type, MapType::KeyType(left))) {
				return false;
			}
			if (!UpgradeType(key_type, MapType::KeyType(right))) {
				return false;
			}

			// Value Type
			LogicalType value_type = LogicalType::SQLNULL;
			if (!UpgradeType(value_type, MapType::ValueType(left))) {
				return false;
			}
			if (!UpgradeType(value_type, MapType::ValueType(right))) {
				return false;
			}
			left = LogicalType::MAP(key_type, value_type);
		} else if (right.id() == LogicalTypeId::STRUCT) {
			auto value_type = MapType::ValueType(left);
			if (!CombineStructTypes(value_type, right)) {
				return false;
			}
			left = LogicalType::MAP(LogicalType::VARCHAR, value_type);
		} else {
			return false;
		}
		return true;
	}
	default: {
		if (!CheckTypeCompatibility(left, right)) {
			return false;
		}
		left = LogicalType::ForceMaxLogicalType(left, right);
		return true;
	}
	}
}

LogicalType PandasAnalyzer::GetListType(py::object &ele, bool &can_convert) {
	auto size = py::len(ele);

	if (size == 0) {
		return LogicalType::SQLNULL;
	}

	idx_t i = 0;
	LogicalType list_type = LogicalType::SQLNULL;
	for (auto py_val : ele) {
		auto object = py::reinterpret_borrow<py::object>(py_val);
		auto item_type = GetItemType(object, can_convert);
		if (!i) {
			list_type = item_type;
		} else {
			if (!UpgradeType(list_type, item_type)) {
				can_convert = false;
			}
		}
		if (!can_convert) {
			break;
		}
		i++;
	}
	return list_type;
}

static LogicalType EmptyMap() {
	return LogicalType::MAP(LogicalTypeId::SQLNULL, LogicalTypeId::SQLNULL);
}

//! Check if the keys match
static bool StructKeysAreEqual(idx_t row, const child_list_t<LogicalType> &reference,
                               const child_list_t<LogicalType> &compare) {
	D_ASSERT(reference.size() == compare.size());
	for (idx_t i = 0; i < reference.size(); i++) {
		auto &ref = reference[i].first;
		auto &comp = compare[i].first;
		if (!duckdb::CaseInsensitiveStringEquality()(ref, comp)) {
			return false;
		}
	}
	return true;
}

// Verify that all struct entries in a column have the same amount of fields and that keys are equal
static bool VerifyStructValidity(vector<LogicalType> &structs) {
	D_ASSERT(!structs.empty());
	idx_t reference_entry = 0;
	// Get first non-null entry
	for (; reference_entry < structs.size(); reference_entry++) {
		if (structs[reference_entry].id() != LogicalTypeId::SQLNULL) {
			break;
		}
	}
	// All entries are NULL
	if (reference_entry == structs.size()) {
		return true;
	}
	auto reference_type = structs[reference_entry];
	auto reference_children = StructType::GetChildTypes(reference_type);

	for (idx_t i = reference_entry + 1; i < structs.size(); i++) {
		auto &entry = structs[i];
		if (entry.id() == LogicalTypeId::SQLNULL) {
			continue;
		}
		auto &entry_children = StructType::GetChildTypes(entry);
		if (entry_children.size() != reference_children.size()) {
			return false;
		}
		if (!StructKeysAreEqual(i, reference_children, entry_children)) {
			return false;
		}
	}
	return true;
}

LogicalType PandasAnalyzer::DictToMap(const PyDictionary &dict, bool &can_convert) {
	auto keys = dict.values.attr("__getitem__")(0);
	auto values = dict.values.attr("__getitem__")(1);

	if (py::none().is(keys) || py::none().is(values)) {
		return LogicalType::MAP(LogicalTypeId::SQLNULL, LogicalTypeId::SQLNULL);
	}

	auto key_type = GetListType(keys, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}
	auto value_type = GetListType(values, can_convert);
	if (!can_convert) {
		return EmptyMap();
	}

	return LogicalType::MAP(key_type, value_type);
}

//! Python dictionaries don't allow duplicate keys, so we don't need to check this.
LogicalType PandasAnalyzer::DictToStruct(const PyDictionary &dict, bool &can_convert) {
	child_list_t<LogicalType> struct_children;

	for (idx_t i = 0; i < dict.len; i++) {
		auto dict_key = dict.keys.attr("__getitem__")(i);

		//! Have to already transform here because the child_list needs a string as key
		auto key = string(py::str(dict_key));

		auto dict_val = dict.values.attr("__getitem__")(i);
		auto val = GetItemType(dict_val, can_convert);
		struct_children.push_back(make_pair(key, std::move(val)));
	}
	return LogicalType::STRUCT(struct_children);
}

//! 'can_convert' is used to communicate if internal structures encountered here are valid
//! e.g python lists can consist of multiple different types, which we cant communicate downwards through
//! LogicalType's alone

LogicalType PandasAnalyzer::GetItemType(py::object ele, bool &can_convert) {
	auto object_type = GetPythonObjectType(ele);

	switch (object_type) {
	case PythonObjectType::None:
		return LogicalType::SQLNULL;
	case PythonObjectType::Bool:
		return LogicalType::BOOLEAN;
	case PythonObjectType::Integer: {
		Value integer;
		if (!TryTransformPythonNumeric(integer, ele)) {
			can_convert = false;
			return LogicalType::SQLNULL;
		}
		return integer.type();
	}
	case PythonObjectType::Float:
		if (std::isnan(PyFloat_AsDouble(ele.ptr()))) {
			return LogicalType::SQLNULL;
		}
		return LogicalType::DOUBLE;
	case PythonObjectType::Decimal: {
		PyDecimal decimal(ele);
		LogicalType type;
		if (!decimal.TryGetType(type)) {
			can_convert = false;
		}
		return type;
	}
	case PythonObjectType::Datetime: {
		auto tzinfo = ele.attr("tzinfo");
		if (!py::none().is(tzinfo)) {
			return LogicalType::TIMESTAMP_TZ;
		}
		return LogicalType::TIMESTAMP;
	}
	case PythonObjectType::Time: {
		auto tzinfo = ele.attr("tzinfo");
		if (!py::none().is(tzinfo)) {
			return LogicalType::TIME_TZ;
		}
		return LogicalType::TIME;
	}
	case PythonObjectType::Date:
		return LogicalType::DATE;
	case PythonObjectType::Timedelta:
		return LogicalType::INTERVAL;
	case PythonObjectType::String:
		return LogicalType::VARCHAR;
	case PythonObjectType::Uuid:
		return LogicalType::UUID;
	case PythonObjectType::ByteArray:
	case PythonObjectType::MemoryView:
	case PythonObjectType::Bytes:
		return LogicalType::BLOB;
	case PythonObjectType::Tuple:
	case PythonObjectType::List:
		return LogicalType::LIST(GetListType(ele, can_convert));
	case PythonObjectType::Dict: {
		PyDictionary dict = PyDictionary(py::reinterpret_borrow<py::object>(ele));
		// Assuming keys and values are the same size

		if (dict.len == 0) {
			return EmptyMap();
		}
		if (DictionaryHasMapFormat(dict)) {
			return DictToMap(dict, can_convert);
		}
		return DictToStruct(dict, can_convert);
	}
	case PythonObjectType::NdDatetime: {
		return GetItemType(ele.attr("tolist")(), can_convert);
	}
	case PythonObjectType::NdArray: {
		auto extended_type = ConvertNumpyType(ele.attr("dtype"));
		LogicalType ltype;
		ltype = NumpyToLogicalType(extended_type);
		if (extended_type.type == NumpyNullableType::OBJECT) {
			LogicalType converted_type = InnerAnalyze(ele, can_convert, 1);
			if (can_convert) {
				ltype = converted_type;
			}
		}
		return LogicalType::LIST(ltype);
	}
	case PythonObjectType::Other:
		// Fall back to string for unknown types
		can_convert = false;
		return LogicalType::VARCHAR;
	default:
		throw InternalException("Unsupported PythonObjectType");
	}
}

//! Get the increment for the given sample size
uint64_t PandasAnalyzer::GetSampleIncrement(idx_t rows) {
	//! Apply the maximum
	auto sample = sample_size;
	if (sample > rows) {
		sample = rows;
	}
	if (sample == 0) {
		return rows;
	}
	return rows / sample;
}

LogicalType PandasAnalyzer::InnerAnalyze(py::object column, bool &can_convert, idx_t increment) {
	idx_t rows = py::len(column);

	if (rows == 0) {
		return LogicalType::SQLNULL;
	}
	auto &import_cache = *DuckDBPyConnection::ImportCache();
	auto pandas_series = import_cache.pandas.Series();

	// Keys are not guaranteed to start at 0 for Series, use the internal __array__ instead
	if (pandas_series && py::isinstance(column, pandas_series)) {
		// TODO: check if '_values' is more portable, and behaves the same as '__array__()'
		column = column.attr("__array__")();
	}
	auto row = column.attr("__getitem__");

	LogicalType item_type = LogicalType::SQLNULL;
	vector<LogicalType> types;
	for (idx_t i = 0; i < rows; i += increment) {
		auto obj = row(i);
		auto next_item_type = GetItemType(obj, can_convert);
		types.push_back(next_item_type);

		if (!can_convert || !UpgradeType(item_type, next_item_type)) {
			can_convert = false;
			return next_item_type;
		}
	}

	if (can_convert && item_type.id() == LogicalTypeId::STRUCT) {
		can_convert = VerifyStructValidity(types);
	}

	return item_type;
}

bool PandasAnalyzer::Analyze(py::object column) {
	// Disable analyze
	if (sample_size == 0) {
		return false;
	}
	auto &import_cache = *DuckDBPyConnection::ImportCache();
	auto pandas = import_cache.pandas();
	if (!pandas) {
		//! Pandas is not installed, no need to analyze
		return false;
	}

	bool can_convert = true;
	idx_t increment = GetSampleIncrement(py::len(column));
	LogicalType type = InnerAnalyze(column, can_convert, increment);

	if (type == LogicalType::SQLNULL && increment > 1) {
		// We did not see the whole dataset, hence we are not sure if nulls are really nulls
		// as a fallback we try to identify this specific type
		auto first_valid_index = column.attr("first_valid_index")();
		if (GetPythonObjectType(first_valid_index) != PythonObjectType::None) {
			// This means we do have a value that is not null, figure out its type
			auto row = column.attr("__getitem__");
			auto obj = row(first_valid_index);
			type = GetItemType(obj, can_convert);
		}
	}
	if (can_convert) {
		analyzed_type = type;
	}
	return can_convert;
}

} // namespace duckdb
