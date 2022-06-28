#pragma once

#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <mutex>

namespace stupid {

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
		ref_count_--;

		if (ref_count_ == 1)
		{
			book_->dispose(this);
		}
	}

	bool is_dangling() const
	{
		return ref_count_ == 0;
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

	Immutable(Immutable<T> && rhs) noexcept
	{
		if (record_) record_->unref();
		
		record_ = rhs.record_;

		rhs.record_ = nullptr;
	}

	Immutable(const Immutable<T>& rhs)
	{
		if (record_) record_->unref();

		record_ = rhs.record_;

		if (record_) record_->ref();
	}

	Immutable<T>& operator=(const Immutable<T>& rhs)
	{
		if (record_) record_->unref();

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

	T* get_data()
	{
		assert(record_);

		return record_->get_data();
	}

	const T* operator->() const { return get_data(); }
	const T& operator*() const { return *(get_data()); }

	T* operator->() { return get_data(); }
	T& operator*() { return *(get_data()); }

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

		assert(dispose_flags_.empty() && "A stupid::Object is being destructed but there are still dangling references to it. Make sure all stupid::Immutable's for this object have been deleted before stupid::Object is destructed.");
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
			pos->second = true;
		}
	}

	void collect()
	{
		for (auto pos = dispose_flags_.begin(); pos != dispose_flags_.end();)
		{
			const auto record{ pos->first };
			const auto disposed { pos->second };

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

	Immutable<T> get() { return object_->get(); }

private:

	Read(Object<T>* object) : object_(object) {}

	Object<T>* object_;
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

	Immutable<T> get() { return object_->get(); }
	const Immutable<T> get() const { return object_->get(); }

private:

	Write(Object<T>* object) : object_(object) {}

	Object<T>* object_;
};

template <class T>
class Object
{
	friend class Read<T>;
	friend class Write<T>;

public:

	Object() : read_(this) , write_(this) {}

	Read<T>& read() { return read_; }
	const Read<T>& read() const { return read_; }

	Write<T>& write() { return write_; }
	const Write<T>& write() const { return write_; }

	bool has_data() const { return last_written_record_; }

private:

	Immutable<T> get() const
	{
		return Immutable<T> { last_written_record_ };
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

		last_written_record_ = record;
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

		new_data_ = true;

		return out;
	}

	template <class ... Args>
	Immutable<T> commit_new(Args... args)
	{
		const auto out = object_.write().commit(new T(args...));

		new_data_ = true;

		return out;
	}

	Read<T>& read() { return object_.read(); }
	const Read<T>& read() const { return object_.read(); }

	bool pending() const { return new_data_; }

private:

	void update()
	{
		const auto signal_value = signal_->get_value();

		if (signal_value > slot_value_)
		{
			const auto new_data = new_data_.exchange(false);

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
	void update(std::int8_t idx)
	{
		assert(idx == 0 || idx == 1);

		const auto signal_value = signal_->get_value();

		if (signal_value > slot_value_)
		{
			const auto new_data = new_data_.exchange(false);

			if (new_data)
			{
				retrieved_[idx] = object_.read().get();
			}
		}

		slot_value_ = signal_value;
	}

	// Get the current data for [0|1].
	const T& get_data(std::int8_t idx)
	{
		assert(idx == 0 || idx == 1);

		if (retrieved_[idx]) return *retrieved_[idx];
		if (retrieved_[flip(idx)]) return *retrieved_[flip(idx)];

		update(idx);

		// Could trip if nothing has been committed yet.
		assert(retrieved_[idx]);

		return *retrieved_[idx];
	}

	T* copy() const
	{
		return object_.write().copy();
	}

	Immutable<T> commit(T* data)
	{
		const auto out = object_.write().commit(data);

		new_data_ = true;

		return out;
	}

	template <class ... Args>
	Immutable<T> commit_new(Args... args)
	{
		const auto out = object_.write().commit(new T(args...));

		new_data_ = true;

		return out;
	}

	Read<T>& read() { return object_.read(); }
	const Read<T>& read() const { return object_.read(); }

	bool pending() const { return new_data_; }

private:

	static auto flip(std::int8_t x) { return 1 - x; }

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

struct AtomicTrigger
{
	AtomicTrigger(std::memory_order memory_order = std::memory_order_relaxed)
		: memory_order_ { memory_order }
	{
		flag_.test_and_set(memory_order);
	}

	void operator()()
	{
		flag_.clear(memory_order_);
	}

	operator bool()
	{
		return !(flag_.test_and_set(memory_order_));
	}

private:

	std::memory_order memory_order_;
	std::atomic_flag flag_;
};

//
// Ball thrown between two players
//
// Can be used to coordinate access to some memory between two threads
//
// Only the player currently holding the ball is allowed to access the
// memory
//
// Each player must poll by calling catch_ball(), to check
// if the ball has been thrown back to them yet
//
// Calling throw_ball() when you don't have the ball is invalid
//
// throw_ball() performs a release store
// catch_ball() performs an acquire load (if it succeeds)
//
class BeachBall
{
public:

	BeachBall(int first_catcher)
	{
		assert(first_catcher == 0 || first_catcher == 1);

		thrown_to_.store(first_catcher, std::memory_order_relaxed);
	}

	// We're not allowed to call this unless we have the ball,
	// i.e. catch_ball() must have returned true since our
	// last call to throw_ball().
	template <int player>
	auto throw_ball() -> void
	{
		static_assert(player == 0 || player == 1);

		thrown_to_.store(1 - player, std::memory_order_release);
	}

	// Returns true if the ball is caught
	// 
	// Returns false if the ball has not been thrown to this player.
	// 
	// May also return false spuriously because that's how
	// compare_exchange_weak() works, but will always return true
	// eventually if the ball has been thrown to us.
	template <int player>
	auto catch_ball() -> bool
	{
		static_assert(player == 0 || player == 1);

		int tmp{ player };

		return thrown_to_.compare_exchange_weak(tmp, NO_PLAYER, std::memory_order_acquire, std::memory_order_relaxed);
	}

private:

	static inline constexpr int NO_PLAYER{ -1 };

	std::atomic<int> thrown_to_;
};

template <int player>
class BeachBallPlayer
{
public:

	BeachBall* const ball;

	BeachBallPlayer(BeachBall* ball_)
		: ball{ ball_ }
	{
		static_assert(player == 0 || player == 1);
	}

	auto throw_ball() -> void
	{
		assert(have_ball_);

		have_ball_ = false;
		ball->throw_ball<player>();
	}

	auto catch_ball() -> bool
	{
		assert(!have_ball_);

		if (ball->catch_ball<player>())
		{
			have_ball_ = true;
		}

		return have_ball_;
	}

	auto have_ball() const -> bool
	{
		return have_ball_;
	}

	auto ensure() -> bool
	{
		if (!have_ball_)
		{
			if (!catch_ball()) return false;
		}

		return true;
	}

private:

	bool have_ball_{};
};

} // stupid