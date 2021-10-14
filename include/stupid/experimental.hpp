#pragma once

#include <atomic>
#include <map>

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
	}

	operator bool() const { return record_; }

	const T* get() const
	{
		assert(record_);

		return record_->object;
	}

	const T* operator->() const { return get(); }
	const T& operator*() const { return *(get()); }

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

			if (disposed && record->ref_count.load() == 0)
			{
				delete record->object;
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

	void update()
	{
		if (object_->pending())
		{
			object_->get();
		}
	}

	Immutable<T> get() const
	{
		return retrieved_;
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

	T* copy() { return object_->copy(); }
	void commit(T* data) { object_->commit(data); }

	template <class ... Args>
	void commit_new(Args... args) { object_->commit(new T(args...)); }

	Immutable<T> get() { return Immutable<T> { object_->last_written_record_.load(std::memory_order::memory_order_relaxed) }; }
	const Immutable<T> get() const { return Immutable<T> { object_->last_written_record_.load(std::memory_order::memory_order_relaxed) }; }

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

	bool pending() const { return pending_record_.load(); }

private:

	Immutable<T> get()
	{
		assert(pending());

		return Immutable<T> { pending_record_.exchange(nullptr) };
	}

	T* copy()
	{
		Immutable<T> ref{ last_written_record_.load(std::memory_order::memory_order_relaxed) };

		if (!ref) return nullptr;

		return new T(*(ref.get()));
	}

	void commit(T* data)
	{
		const auto record = book_.make_record(data);

		pending_record_.store(record);
		last_written_record_.store(record, std::memory_order::memory_order_relaxed);
		pending_ref_ = Immutable<T>{ record };
		last_written_ = Immutable<T>{ record };

		book_.collect();
	}

	Read<T> read_;
	Write<T> write_;

	Book<T> book_;

	std::atomic<Record<T>*> pending_record_{ nullptr };
	std::atomic<Record<T>*> last_written_record_{ nullptr };

	// Keep at least one reference until overwritten
	Immutable<T> pending_ref_;
	Immutable<T> last_written_ref_;
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
		if (!retrieved_) update();

		return *retrieved_;
	}

	Write<T>& write() { return object_.write(); }
	const Write<T>& write() const { return object_.write(); }

private:

	void update()
	{
		const auto signal_value = signal_->get_value();

		if (signal_value > slot_value_ && object_.pending())
		{
			object_.read().update();
			retrieved_ = object_.read().get();
		}

		slot_value_ = signal_value;
	}

	Object<T> object_;
	SignalType* signal_;
	std::uint32_t slot_value_{ 0 };
	Immutable<T> retrieved_;
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

		if (signal_value > slot_value_ && object_.pending())
		{
			retrieved_[idx] = object_.read().update();
			recent_[idx] = retrieved_[idx];
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

	Write<T>& write() { return object_.write(); }
	const Write<T>& write() const { return object_.write(); }

private:

	static int flip(int x) { return 1 - x; }

	Object<T> object_;
	SignalType* signal_;
	std::uint32_t slot_value_{ 0 };
	std::array<Immutable<T>, 2> retrieved_;
};

template <class T, class SignalType = SyncSignal>
class QuickSync
{
public:

	QuickSync(const SignalType& signal)
		: object_(signal)
	{
		object_.write().commit_new();
	}

	void sync_copy(std::function<void(T*)> mutator)
	{
		const auto copy = object_.write().copy();

		mutator(copy);

		object_.write().commit(copy);
	}

	void sync_new(std::function<void(T*)> mutator)
	{
		const auto new_data = new T();

		mutator(new_data.get());

		object_.write().commit(new_data);
	}
	
	const T& get_data()
	{
		return object_.get_data();
	}

private:

	SignalSyncObject<T> object_;
};

} // experimental
} // stupid