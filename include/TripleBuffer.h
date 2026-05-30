#pragma once
#include <atomic>
#include <algorithm>
#include <memory>

// MPMC Lock-Free Buffer
// Eventually consistent reads via triple buffering
// Suitable for: short read operations, latest-snapshot workloads
// Not suitable for: long read operations, ordered update processing

namespace jei {
	template <typename T>
	struct BufferReader;

	template <typename T>
	class TripleBuffer {
		T* back1;
		T* back2; // reader references this
		std::atomic<uint16_t> flag = 0;

		// flag layout (16bit):
		// [15]    write lock  : set during back1 swap
		// [14]    fresh       : back2 holds latest data (cleared on write)
		// [13:0]  reader count: active readers referencing back2
		//
		// swap condition: flag == 0 (not fresh, no readers)
		// ABA-safe: flag cycling back to 0 implies a new write occurred

	public:
		void Init(T* b1, T* b2) {
			back1 = b1;
			back2 = b2;
		}
		void Write(T* write) {
			while (true)
			{
				uint16_t old_flag = flag.load(std::memory_order_relaxed);
				uint16_t flagNotUsing = old_flag & (0x8000 - 1); // mask: 001111...
				uint16_t flagUsing = old_flag | 0x8000;
				if (flag.compare_exchange_weak(flagNotUsing, flagUsing, std::memory_order_acq_rel, std::memory_order_relaxed))
					break;
			}

			std::swap(back1, write);

			while (true)
			{
				uint16_t old_flag = flag.load(std::memory_order_relaxed);
				uint16_t flagNotUsing = old_flag & (0x4000 - 1);
				if (flag.compare_exchange_weak(old_flag, flagNotUsing, std::memory_order_acq_rel, std::memory_order_relaxed))
					break;
			}
		}

		BufferReader<T> Read() {
			uint16_t new_flag;

			while (true) {
				uint16_t old_flag = flag.load(std::memory_order_relaxed);
				if (old_flag == 0) {
					// not fresh + no readers: take swap ownership
					new_flag = 0x8000;         
				}
				else if (old_flag & 0x8000) {
					// block during write or swap in progress
					continue;
				}
				else {
					new_flag = old_flag + 1;  
				}

				if (flag.compare_exchange_weak(old_flag, new_flag, std::memory_order_acq_rel, std::memory_order_relaxed)) {
					break; 
				}
			}

			if (new_flag == 0x8000) {
				std::swap(back1, back2);
				flag.store(0x4000 + 1, std::memory_order_release);
			}

			return BufferReader<T>(back2, this);
		}
		void ReadDone() {
			flag.fetch_sub(1, std::memory_order_relaxed);
		}
	};


	template <typename T>
	struct BufferReader {
		TripleBuffer<T>* owner;
		T* data;
		BufferReader(T* ptr, TripleBuffer<T>* o) {
			data = ptr;
			owner = o;
		}
		~BufferReader() {
			if (owner != nullptr)
				owner->ReadDone();
		}

		BufferReader(const BufferReader&) = delete;
		BufferReader& operator=(const BufferReader&) = delete;
		// non-copyable, movable
		BufferReader(BufferReader&& other) noexcept
			: owner(other.owner), data(other.data) {
			other.owner = nullptr;
			other.data = nullptr;
		}

		BufferReader& operator=(BufferReader&& other) noexcept {
			if (this != &other) {
				owner = other.owner;
				data = other.data;
				other.owner = nullptr;
				other.data = nullptr;
			}
			return *this;
		}
	};
}