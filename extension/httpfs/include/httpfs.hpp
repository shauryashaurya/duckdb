#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace httplib {
struct Response;
struct Client;
}
namespace duckdb {

using HeaderMap = unordered_map<string, string>;

// avoid including httplib in header
struct ClientDeleter {
	void operator()(httplib::Client* client);
};

// avoid including httplib in header
struct ResponseWrapper {
public:
	explicit ResponseWrapper(httplib::Response &res);
	int code;
	string error;
	HeaderMap headers;
};

class HTTPFileHandle : public FileHandle {
public:
	HTTPFileHandle(FileSystem &fs, std::string path, uint8_t flags);
	// This two-phase construction allows subclasses more flexible setup.
	virtual unique_ptr<ResponseWrapper> Initialize();

	// We keep an http client stored for connection reuse with keep-alive headers
	unique_ptr<httplib::Client, ClientDeleter> http_client = nullptr;

	// File handle info
	uint8_t flags;
	idx_t length;
	time_t last_modified;

	// Read info
	idx_t buffer_available;
	idx_t buffer_idx;
	idx_t file_offset;
	idx_t buffer_start;
	idx_t buffer_end;

	// Read buffer
	std::unique_ptr<data_t[]> read_buffer;
	constexpr static idx_t READ_BUFFER_LEN = 1000000;

public:
	void Close() override {}
};

class HTTPFileSystem : public FileSystem {
public:
	static constexpr int HTTP_WRITE_TIMEOUT_SEC = 60;
	static constexpr int HTTP_READ_TIMEOUT_SEC = 60;
	static constexpr int HTTP_CONNECTION_TIMEOUT_SEC = 60;

	std::unique_ptr<FileHandle> OpenFile(const string &path, uint8_t flags, FileLockType lock = DEFAULT_LOCK,
	                                     FileCompressionType compression = DEFAULT_COMPRESSION,
	                                     FileOpener *opener = nullptr) final;

	std::vector<std::string> Glob(const std::string &path) override {
		return {path}; // FIXME
	}

	// HTTP Requests
	virtual unique_ptr<ResponseWrapper> PutRequest(FileHandle &handle, string url, HeaderMap header_map, char* buffer_in, idx_t buffer_in_len);
	virtual unique_ptr<ResponseWrapper> HeadRequest(FileHandle &handle, string url,HeaderMap header_map);
	// Get Request with range parameter that GETs exactly buffer_out_len bytes from the url
	virtual unique_ptr<ResponseWrapper> GetRangeRequest(FileHandle &handle, string url, HeaderMap header_map, idx_t file_offset, char *buffer_out, idx_t buffer_out_len);
	// Post Request that can handle variable sized responses without a content-length header, which is necessary for S3 Multipart
	virtual unique_ptr<ResponseWrapper> PostRequest(FileHandle &handle, string url,HeaderMap header_map, unique_ptr<char[]>& buffer_out, idx_t &buffer_out_len, char* buffer_in, idx_t buffer_in_len);

	// FS methods
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void FileSync(FileHandle &handle) override;
	int64_t GetFileSize(FileHandle &handle) override;
	time_t GetLastModifiedTime(FileHandle &handle) override;
	bool FileExists(const string &filename) override;
	void Seek(FileHandle &handle, idx_t location) override;
	bool CanHandleFile(const string &fpath) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	std::string GetName() const override {
		return "HTTPFileSystem";
	}

	static void Verify();

protected:
	virtual std::unique_ptr<HTTPFileHandle> CreateHandle(const string &path, uint8_t flags, FileLockType lock,
	                                                     FileCompressionType compression, FileOpener *opener);
};

} // namespace duckdb
