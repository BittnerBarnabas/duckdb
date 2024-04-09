//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/buffer_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/storage/buffer/temporary_file_information.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

class Allocator;
class BufferPool;
class TemporaryMemoryManager;

class BufferManager {
	friend class BufferHandle;
	friend class BlockHandle;
	friend class BlockManager;

public:
	BufferManager() {
	}
	virtual ~BufferManager() {
	}

public:
	virtual BufferHandle Allocate(MemoryTag tag, idx_t block_size, bool can_destroy = true,
	                              shared_ptr<BlockHandle> *block = nullptr) = 0;
	//! Reallocate an in-memory buffer that is pinned.
	virtual void ReAllocate(shared_ptr<BlockHandle> &handle, idx_t block_size) = 0;
	virtual BufferHandle Pin(shared_ptr<BlockHandle> &handle) = 0;
	virtual void Unpin(shared_ptr<BlockHandle> &handle) = 0;
	//! Returns the currently allocated memory
	virtual idx_t GetUsedMemory() const = 0;
	//! Returns the maximum available memory
	virtual idx_t GetMaxMemory() const = 0;
	virtual shared_ptr<BlockHandle> RegisterSmallMemory(idx_t block_size);
	virtual DUCKDB_API Allocator &GetBufferAllocator();
	virtual DUCKDB_API void ReserveMemory(idx_t size);
	virtual DUCKDB_API void FreeReservedMemory(idx_t size);
	virtual vector<MemoryInformation> GetMemoryUsageInfo() const = 0;
	//! Set a new memory limit to the buffer manager, throws an exception if the new limit is too low and not enough
	//! blocks can be evicted
	virtual void SetLimit(idx_t limit = (idx_t)-1);
	virtual vector<TemporaryFileInformation> GetTemporaryFiles();
	virtual const string &GetTemporaryDirectory() const;
	virtual void SetTemporaryDirectory(const string &new_dir);
	virtual bool HasTemporaryDirectory() const;
	//! Construct a managed buffer.
	virtual unique_ptr<FileBuffer> ConstructManagedBuffer(idx_t size, unique_ptr<FileBuffer> &&source,
	                                                      FileBufferType type = FileBufferType::MANAGED_BUFFER);
	//! Get the underlying buffer pool responsible for managing the buffers
	virtual BufferPool &GetBufferPool() const;
	//! Get the manager that assigns reservations for temporary memory, e.g., for query intermediates
	virtual TemporaryMemoryManager &GetTemporaryMemoryManager();

	// Static methods
	DUCKDB_API static BufferManager &GetBufferManager(DatabaseInstance &db);
	DUCKDB_API static const BufferManager &GetBufferManager(const DatabaseInstance &db);
	DUCKDB_API static BufferManager &GetBufferManager(ClientContext &context);
	DUCKDB_API static const BufferManager &GetBufferManager(const ClientContext &context);
	DUCKDB_API static BufferManager &GetBufferManager(AttachedDatabase &db);

	static idx_t GetAllocSize(idx_t block_size) {
		return AlignValue<idx_t, Storage::SECTOR_SIZE>(block_size + Storage::BLOCK_HEADER_SIZE);
	}
	//! Returns the maximum available memory for a given query
	idx_t GetQueryMaxMemory() const;

protected:
	virtual void PurgeQueue(FileBufferType type) = 0;
	virtual void AddToEvictionQueue(shared_ptr<BlockHandle> &handle);
	virtual void WriteTemporaryBuffer(MemoryTag tag, block_id_t block_id, FileBuffer &buffer);
	virtual unique_ptr<FileBuffer> ReadTemporaryBuffer(MemoryTag tag, block_id_t id, unique_ptr<FileBuffer> buffer);
	virtual void DeleteTemporaryFile(block_id_t id);
};

} // namespace duckdb
