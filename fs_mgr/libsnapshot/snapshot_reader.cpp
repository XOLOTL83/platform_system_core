//
// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "snapshot_reader.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <ext4_utils/ext4_utils.h>

namespace android {
namespace snapshot {

using android::base::borrowed_fd;

// Not supported.
bool ReadOnlyFileDescriptor::Open(const char*, int, mode_t) {
    errno = EINVAL;
    return false;
}

bool ReadOnlyFileDescriptor::Open(const char*, int) {
    errno = EINVAL;
    return false;
}

ssize_t ReadOnlyFileDescriptor::Write(const void*, size_t) {
    errno = EINVAL;
    return false;
}

bool ReadOnlyFileDescriptor::BlkIoctl(int, uint64_t, uint64_t, int*) {
    errno = EINVAL;
    return false;
}

ReadFdFileDescriptor::ReadFdFileDescriptor(android::base::unique_fd&& fd) : fd_(std::move(fd)) {}

ssize_t ReadFdFileDescriptor::Read(void* buf, size_t count) {
    return read(fd_.get(), buf, count);
}

off64_t ReadFdFileDescriptor::Seek(off64_t offset, int whence) {
    return lseek(fd_.get(), offset, whence);
}

uint64_t ReadFdFileDescriptor::BlockDevSize() {
    return get_block_device_size(fd_.get());
}

bool ReadFdFileDescriptor::Close() {
    fd_ = {};
    return true;
}

bool ReadFdFileDescriptor::IsSettingErrno() {
    return true;
}

bool ReadFdFileDescriptor::IsOpen() {
    return fd_ >= 0;
}

bool ReadFdFileDescriptor::Flush() {
    return true;
}

bool CompressedSnapshotReader::SetCow(std::unique_ptr<CowReader>&& cow) {
    cow_ = std::move(cow);

    const auto& header = cow_->GetHeader();
    block_size_ = header.block_size;

    // Populate the operation map.
    op_iter_ = cow_->GetOpIter();
    while (!op_iter_->AtEnd()) {
        const CowOperation* op = op_iter_->Get();
        if (IsMetadataOp(*op)) {
            op_iter_->Next();
            continue;
        }
        if (op->new_block >= ops_.size()) {
            ops_.resize(op->new_block + 1, nullptr);
        }
        ops_[op->new_block] = op;
        op_iter_->Next();
    }

    return true;
}

void CompressedSnapshotReader::SetSourceDevice(const std::string& source_device) {
    source_device_ = {source_device};
}

void CompressedSnapshotReader::SetBlockDeviceSize(uint64_t block_device_size) {
    block_device_size_ = block_device_size;
}

borrowed_fd CompressedSnapshotReader::GetSourceFd() {
    if (source_fd_ < 0) {
        if (!source_device_) {
            LOG(ERROR) << "CompressedSnapshotReader needs source device, but none was set";
            errno = EINVAL;
            return {-1};
        }
        source_fd_.reset(open(source_device_->c_str(), O_RDONLY | O_CLOEXEC));
        if (source_fd_ < 0) {
            PLOG(ERROR) << "open " << *source_device_;
            return {-1};
        }
    }
    return source_fd_;
}

ssize_t CompressedSnapshotReader::Read(void* buf, size_t count) {
    // Find the start and end chunks, inclusive.
    uint64_t start_chunk = offset_ / block_size_;
    uint64_t end_chunk = (offset_ + count - 1) / block_size_;

    // Chop off the first N bytes if the position is not block-aligned.
    size_t start_offset = offset_ % block_size_;

    uint8_t* buf_pos = reinterpret_cast<uint8_t*>(buf);
    size_t buf_remaining = count;

    size_t initial_bytes = std::min(block_size_ - start_offset, buf_remaining);
    ssize_t rv = ReadBlock(start_chunk, start_offset, buf_pos, initial_bytes);
    if (rv < 0) {
        return -1;
    }
    offset_ += rv;
    buf_pos += rv;
    buf_remaining -= rv;

    for (uint64_t chunk = start_chunk + 1; chunk < end_chunk; chunk++) {
        ssize_t rv = ReadBlock(chunk, 0, buf_pos, buf_remaining);
        if (rv < 0) {
            return -1;
        }
        offset_ += rv;
        buf_pos += rv;
        buf_remaining -= rv;
    }

    if (buf_remaining) {
        ssize_t rv = ReadBlock(end_chunk, 0, buf_pos, buf_remaining);
        if (rv < 0) {
            return -1;
        }
        offset_ += rv;
        buf_pos += rv;
        buf_remaining -= rv;
    }

    CHECK_EQ(buf_pos - reinterpret_cast<uint8_t*>(buf), count);
    CHECK_EQ(buf_remaining, 0);

    errno = 0;
    return count;
}

ssize_t CompressedSnapshotReader::ReadBlock(uint64_t chunk, size_t start_offset, void* buffer,
                                            size_t buffer_size) {
    size_t bytes_to_read = std::min(static_cast<size_t>(block_size_), buffer_size);

    // The offset is relative to the chunk; we should be reading no more than
    // one chunk.
    CHECK(start_offset + bytes_to_read <= block_size_);

    const CowOperation* op = nullptr;
    if (chunk < ops_.size()) {
        op = ops_[chunk];
    }

    if (!op || op->type == kCowCopyOp) {
        borrowed_fd fd = GetSourceFd();
        if (fd < 0) {
            // GetSourceFd sets errno.
            return -1;
        }

        if (op) {
            chunk = op->source;
        }

        off64_t offset = (chunk * block_size_) + start_offset;
        if (!android::base::ReadFullyAtOffset(fd, buffer, bytes_to_read, offset)) {
            PLOG(ERROR) << "read " << *source_device_;
            // ReadFullyAtOffset sets errno.
            return -1;
        }
    } else if (op->type == kCowZeroOp) {
        memset(buffer, 0, bytes_to_read);
    } else if (op->type == kCowReplaceOp) {
        if (cow_->ReadData(op, buffer, bytes_to_read, start_offset) < bytes_to_read) {
            LOG(ERROR) << "CompressedSnapshotReader failed to read replace op";
            errno = EIO;
            return -1;
        }
    } else if (op->type == kCowXorOp) {
        borrowed_fd fd = GetSourceFd();
        if (fd < 0) {
            // GetSourceFd sets errno.
            return -1;
        }

        off64_t offset = op->source + start_offset;

        std::string data(bytes_to_read, '\0');
        if (!android::base::ReadFullyAtOffset(fd, data.data(), data.size(), offset)) {
            PLOG(ERROR) << "read " << *source_device_;
            // ReadFullyAtOffset sets errno.
            return -1;
        }

        if (cow_->ReadData(op, buffer, bytes_to_read, start_offset) < bytes_to_read) {
            LOG(ERROR) << "CompressedSnapshotReader failed to read xor op";
            errno = EIO;
            return -1;
        }

        for (size_t i = 0; i < bytes_to_read; i++) {
            ((char*)buffer)[i] ^= data[i];
        }
    } else {
        LOG(ERROR) << "CompressedSnapshotReader unknown op type: " << uint32_t(op->type);
        errno = EINVAL;
        return -1;
    }

    // MemoryByteSink doesn't do anything in ReturnBuffer, so don't bother calling it.
    return bytes_to_read;
}

off64_t CompressedSnapshotReader::Seek(off64_t offset, int whence) {
    switch (whence) {
        case SEEK_SET:
            offset_ = offset;
            break;
        case SEEK_END:
            offset_ = static_cast<off64_t>(block_device_size_) + offset;
            break;
        case SEEK_CUR:
            offset_ += offset;
            break;
        default:
            LOG(ERROR) << "Unrecognized seek whence: " << whence;
            errno = EINVAL;
            return -1;
    }
    return offset_;
}

uint64_t CompressedSnapshotReader::BlockDevSize() {
    return block_device_size_;
}

bool CompressedSnapshotReader::Close() {
    cow_ = nullptr;
    source_fd_ = {};
    return true;
}

bool CompressedSnapshotReader::IsSettingErrno() {
    return true;
}

bool CompressedSnapshotReader::IsOpen() {
    return cow_ != nullptr;
}

bool CompressedSnapshotReader::Flush() {
    return true;
}

}  // namespace snapshot
}  // namespace android
