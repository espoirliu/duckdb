//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/read_csv.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_state_machine_cache.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"

namespace duckdb {
class BaseScanner;
class StringValueScanner;

class ReadCSV {
public:
	static unique_ptr<CSVFileHandle> OpenCSV(const OpenFileInfo &file, const CSVReaderOptions &options,
	                                         ClientContext &context);
};

struct BaseCSVData : public TableFunctionData {
	//! The CSV reader options
	CSVReaderOptions options;
	//! Offsets for generated columns
	idx_t filename_col_idx {};
	idx_t hive_partition_col_idx {};

	void Finalize();
};

struct WriteCSVData : public BaseCSVData {
	WriteCSVData(string file_path, vector<LogicalType> sql_types, vector<string> names)
	    : sql_types(std::move(sql_types)) {
		files.push_back(std::move(file_path));
		options.name_list = std::move(names);
		if (options.dialect_options.state_machine_options.escape == '\0') {
			options.dialect_options.state_machine_options.escape = options.dialect_options.state_machine_options.quote;
		}
	}

	//! The file path of the CSV file to read or write
	vector<string> files;
	//! The SQL types to write
	vector<LogicalType> sql_types;
	//! The newline string to write
	string newline = "\n";
	//! The size of the CSV file (in bytes) that we buffer before we flush it to disk
	idx_t flush_size = 4096ULL * 8ULL;
	//! For each byte whether the CSV file requires quotes when containing the byte
	unsafe_unique_array<bool> requires_quotes;
	//! Expressions used to convert the input into strings
	vector<unique_ptr<Expression>> cast_expressions;
};

struct ColumnInfo {
	ColumnInfo() {
	}
	ColumnInfo(vector<std::string> names_p, vector<LogicalType> types_p) {
		names = std::move(names_p);
		types = std::move(types_p);
	}
	void Serialize(Serializer &serializer) const;
	static ColumnInfo Deserialize(Deserializer &deserializer);

	vector<std::string> names;
	vector<LogicalType> types;
};

struct ReadCSVData : public BaseCSVData {
	ReadCSVData();
	//! If the sql types from the file were manually set
	vector<bool> manually_set;
	//! The buffer manager (if any): this is used when automatic detection is used during binding.
	//! In this case, some CSV buffers have already been read and can be reused.
	shared_ptr<CSVBufferManager> buffer_manager;
	//! Column info (used for union reader serialization)
	vector<ColumnInfo> column_info;
	//! The CSV schema, in case there is a unified schema that all files must read
	CSVSchema csv_schema;

	void FinalizeRead(ClientContext &context);
};

struct SerializedCSVReaderOptions {
	SerializedCSVReaderOptions() = default;
	SerializedCSVReaderOptions(CSVReaderOptions options, MultiFileOptions file_options);
	SerializedCSVReaderOptions(CSVOption<char> single_byte_delimiter, const CSVOption<string> &multi_byte_delimiter);

	CSVReaderOptions options;
	MultiFileOptions file_options;

	void Serialize(Serializer &serializer) const;
	static SerializedCSVReaderOptions Deserialize(Deserializer &deserializer);
};

struct SerializedReadCSVData {
	vector<string> files;
	vector<LogicalType> csv_types;
	vector<string> csv_names;
	vector<LogicalType> return_types;
	vector<string> return_names;
	idx_t filename_col_idx;
	SerializedCSVReaderOptions options;
	MultiFileReaderBindData reader_bind;
	vector<ColumnInfo> column_info;

	void Serialize(Serializer &serializer) const;
	static SerializedReadCSVData Deserialize(Deserializer &deserializer);
};

struct CSVCopyFunction {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct ReadCSVTableFunction {
	static TableFunction GetFunction();
	static TableFunction GetAutoFunction();
	static void ReadCSVAddNamedParameters(TableFunction &table_function);
	static void RegisterFunction(BuiltinFunctions &set);
};

} // namespace duckdb
