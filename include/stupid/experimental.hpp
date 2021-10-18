#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace stupid {
namespace experimental {

template <class T> class Book;
template <class T> class Object;

template <class T>
class Record
{
public:

	Record(T* data, Book<T>* book)
		: data_(data)
		, book_(book)
	{
	}

	void ref()
	{
		ref_count_++;
	}

	void unref()
	{
		if (--ref_count_ == 0) book_->dispose(this);
	}

	bool is_dangling() const
	{
		return ref_count_.load() == 0;
	}

	T* get_data() { return data_; }
	const T* get_data() const { return data_; }

private:

	T* data_{ nullptr };
	Book<T>* book_{ nullptr };
	std::atomic<int> ref_count_{ 0 };
};

template <class T>
class Immutable
{
public:

	Immutable() = default;

	Immutable(Record<T>* record)
		: record_(record)
	{
		if (record_) record_->ref();
	}

	~Immutable()
	{
		if (record_) record_->unref();
	}

	Immutable(const Immutable<T>& rhs)
		: record_(rhs.record_)
	{
		if (record_) record_->ref();
	}

	Immutable<T>& operator=(const Immutable<T>& rhs)
	{
		record_ = rhs.record_;

		if (record_) record_->ref();

		return *this;
	}

	operator bool() const { return record_; }

	const T* get_data() const
	{
		assert(record_);

		return record_->get_data();
	}

	const T* operator->() const { return get_data(); }
	const T& operator*() const { return *(get_data()); }

private:

	Record<T>* record_{ nullptr };
};

template <class T>
class Book
{
public:

	~Book()
	{
		collect();
	}

	Record<T>* make_record(T* data)
	{
		const auto out = new Record<T>{ data, this };

		dispose_flags_[out] = false;

		return out;
	}

	void dispose(Record<T>* record)
	{
		const auto pos = dispose_flags_.find(record);

		if (pos != dispose_flags_.end())
		{
			pos->second.store(true, std::memory_order::memory_order_relaxed);
		}
	}

	void collect()
	{
		for (auto pos = dispose_flags_.begin(); pos != dispose_flags_.end();)
		{
			const auto record = pos->first;
			const auto disposed = pos->second.load(std::memory_order::memory_order_relaxed);

			if (disposed && record->is_dangling())
			{
				delete record->get_data();
				delete record;

				pos = dispose_flags_.erase(pos);
			}
			else
			{
				pos++;
			}
		}
	}

private:

	std::map<Record<T>*, std::atomic_bool> dispose_flags_;
};

template <class T>
class Read
{
	friend class Object<T>;

public:

	Immutable<T> get()
	{
		return (retrieved_ = object_->get());
	}

	const T& get_data() const
	{
		return *retrieved_;
	}

private:

	Read(Object<T>* object)
		: object_(object)
	{
	}

	Object<T>* object_;
	Immutable<T> retrieved_;

};

template <class T>
class Write
{
	friend class Object<T>;

public:

	T* copy() const { return object_->copy(); }
	Immutable<T> commit(T* data) { return object_->commit(data); }

	template <class ... Args>
	Immutable<T> commit_new(Args... args) { return object_->commit(new T(args...)); }

	Immutable<T> get() { object_->get(); }
	const Immutable<T> get() const { object_->get(); }

private:

	Write(Object<T>* object)
		: object_(object)
	{
	}

	Object<T>* object_;
};

template <class T>
class Object
{
	friend class Read<T>;
	friend class Write<T>;

public:

	Object()
		: read_(this)
		, write_(this)
	{
	}

	Read<T>& read() { return read_; }
	const Read<T>& read() const { return read_; }

	Write<T>& write() { return write_; }
	const Write<T>& write() const { return write_; }

	bool has_data() const { return last_written_record_.load(std::memory_order::memory_order_relaxed); }

private:

	Immutable<T> get() const
	{
		return Immutable<T> { last_written_record_.load(std::memory_order::memory_order_relaxed) };
	}

	T* copy() const
	{
		Immutable<T> ref{ get() };

		if (!ref) return nullptr;

		return new T(*(ref.get_data()));
	}

	Immutable<T> commit(T* data)
	{
#if _DEBUG
		std::unique_lock<std::mutex> lock(debug_.commit_mutex, std::try_to_lock);

		if (!lock.owns_lock())
		{
			throw std::runtime_error(
				"stupid::Object::commit() is being called from multiple simultaneous "
				"writer threads which is not supported. If you see this you have a "
				"bug! This check won't be performed in a release build. This exception "
				"won't be thrown in a release build.");
		}
#endif
		const auto record = book_.make_record(data);
		const auto out = Immutable<T>{ record };

		last_written_record_.store(record, std::memory_order::memory_order_relaxed);
		last_written_ref_ = out;

		book_.collect();

		return out;
	}

	Book<T> book_;
	Read<T> read_;
	Write<T> write_;

	std::atomic<Record<T>*> last_written_record_ { nullptr };

	// Keep at least one reference until overwritten
	Immutable<T> last_written_ref_;

#ifdef _DEBUG
	struct
	{
		std::mutex commit_mutex;
	} debug_;
#endif
};

class SyncSignal
{
public:

	std::uint32_t get_value() const { return value_; }
	void operator()() { value_++; }

private:

	std::uint32_t value_ = 0;
};

template <class T, class SignalType = SyncSignal>
class SignalSyncObject
{

public:

	SignalSyncObject(const SignalType& signal)
		: signal_(&signal)
	{
	}

	const T& get_data()
	{
		update();

		return *retrieved_;
	}

	T* copy() const
	{
		return object_.write().copy();
	}

	Immutable<T> commit(T* data)
	{
		const auto out = object_.write().commit(data);

		new_data_.store(true, std::memory_order::memory_order_relaxed);

		return out;
	}

	template <class ... Args>
	Immutable<T> commit_new(Args... args)
	{
		const auto out = object_.write().commit(new T(args...));

		new_data_.store(true, std::memory_order::memory_order_relaxed);

		return out;
	}

private:

	void update()
	{
		const auto signal_value = signal_->get_value();

		if (signal_value > slot_value_)
		{
			const auto new_data = new_data_.exchange(false, std::memory_order::memory_order_relaxed);

			if (new_data)
			{
				retrieved_ = object_.read().get();
			}
		}

		slot_value_ = signal_value;
	}

	Object<T> object_;
	const SignalType* signal_;
	std::uint32_t slot_value_ { 0 };
	Immutable<T> retrieved_;
	std::atomic_bool new_data_ { false };
};

template <class T, class SignalType = SyncSignal>
class SignalSyncObjectPair
{
public:

	SignalSyncObjectPair(const SignalType& signal)
		: signal_(&signal)
	{
	}

	// If there's data pending, store it in [0|1].
	void update(int idx)
	{
		const auto signal_value = signal_->get_value();

		if (signal_value > slot_value_)
		{
			const auto new_data = new_data_.exchange(false, std::memory_order::memory_order_relaxed);

			if (new_data)
			{
				retrieved_[idx] = object_.read().get();
			}
		}

		slot_value_ = signal_value;
	}

	// Get the current data for [0|1].
	const T& get_data(int idx)
	{
		if (retrieved_[idx]) return *retrieved_[idx];
		if (retrieved_[flip(idx)]) return *retrieved_[flip(idx)];

		update(idx);

		return *retrieved_[idx];
	}

	T* copy() const
	{
		return object_.write().copy();
	}

	Immutable<T> commit(T* data)
	{
		const auto out = object_.write().commit(data);

		new_data_.store(true, std::memory_order::memory_order_relaxed);

		return out;
	}

	template <class ... Args>
	Immutable<T> commit_new(Args... args)
	{
		const auto out = object_.write().commit(new T(args...));

		new_data_.store(true, std::memory_order::memory_order_relaxed);

		return out;
	}

	bool pending() const { return new_data_; }

private:

	static int flip(int x) { return 1 - x; }

	Object<T> object_;
	const SignalType* signal_;
	std::uint32_t slot_value_ { 0 };
	std::array<Immutable<T>, 2> retrieved_;
	std::atomic_bool new_data_ { false };
};

template <class T, class SignalType = SyncSignal>
class QuickSync
{
public:

	QuickSync(const SignalType& signal)
		: object_(signal)
	{
		object_.commit_new();
	}

	void sync_copy(std::function<void(T*)> mutator)
	{
		const auto copy = object_.copy();

		mutator(copy);

		object_.commit(copy);
	}

	void sync_new(std::function<void(T*)> mutator)
	{
		const auto new_data = new T();

		mutator(new_data);

		object_.commit(new_data);
	}
	
	const T& get_data()
	{
		return object_.get_data();
	}

private:

	SignalSyncObject<T, SignalType> object_;
};

} // experimental
} // stupid