#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <vector>

namespace stupid {

template <typename T> class ref;

namespace detail {

template <typename T>
struct control_block
{
	const T value;
	std::atomic<uint32_t> ref_count{0};
};

} // detail

template <typename T>
class object
{
public:

	using cb_t = detail::control_block<T>;
	using me_t = object<T>;
	using ref_t = ref<T>;

	object(const object&) = delete;
	auto operator=(const object&) -> object& = delete;

	object(object&& rhs) noexcept = default;
	auto operator=(object&& rhs) noexcept -> object& = default;

	template <typename... Args>
	object(Args... args) : critical_{args...} {}

private:

	struct critical_t
	{
		template <typename... Args>
		critical_t(Args... args) : control_block{new cb_t{T{args...}, 0}} {}

		critical_t(critical_t&& rhs) noexcept
		{
			this->operator=(std::move(rhs));
		}

		auto operator=(critical_t&& rhs) noexcept -> critical_t&
		{
			control_block.store(rhs.control_block.load());
			rhs.control_block.store(nullptr);

			return *this;
		}

		std::atomic<cb_t*> control_block;
	} critical_;

public:

	struct read_t
	{
		read_t(me_t* self) : self_{self} {}
		read_t(read_t&& rhs) noexcept {}

		auto operator=(read_t&& rhs) noexcept -> read_t&
		{
			return *this;
		}

		auto acquire() const -> ref_t
		{
			const auto cb{self_->critical_.control_block.load()};

			assert (cb);

			return ref_t{cb};
		}

		auto get_value() const -> const T&
		{
			const auto cb{self_->critical_.control_block.load()};

			assert (cb);

			return cb->value;
		}

	private:

		me_t* self_;
	} read{this};

	struct write_t
	{
		write_t(me_t* self)
			: self_{self}
		{
			instance_ = ref_t{self_->critical_.control_block.load()};
		}

		write_t(write_t&& rhs) noexcept
		{
			this->operator=(std::move(rhs));
		}

		auto operator=(write_t&& rhs) noexcept -> write_t&
		{
			instance_ = std::move(rhs.instance_);
			garbage_ = std::move(rhs.garbage_);

			return *this;
		}

		template <typename U>
		auto set(U&& value) -> void
		{
			// The old control block won't be reclaimed before
			// the end of this function, because we keep this
			// reference to it. So it won't be garbage collected
			// yet when we call garbage_collect() at the end of
			// this function.
			const auto old_instance{instance_};

			// Create the new control block
			const auto cb{new cb_t{std::forward<U>(value), 0}};

			// Atomically set the new control block
			self_->critical_.control_block = cb;

			// Keep a reference to the new control block
			instance_ = ref_t{cb};

			// Push the old control block onto the garbage. It
			// won't be collected yet.
			garbage_.push_back(old_instance);

			// Collect old control blocks that were already
			// discarded.
			garbage_collect();
		}

		template <typename UpdateFn>
		auto update(UpdateFn&& fn) -> void
		{
			set(fn(*instance_));
		}

	private:

		auto garbage_collect() -> void
		{
			static const auto can_be_deleted = [](const ref_t& instance)
			{
				assert (instance.cb_);
				assert (instance.cb_->ref_count > 0);

				return instance.cb_->ref_count == 1;
			};

			garbage_.erase(std::remove_if(std::begin(garbage_), std::end(garbage_), can_be_deleted), std::end(garbage_));
		}

		me_t* self_;
		ref_t instance_;
		std::vector<ref_t> garbage_;
	} write{this};
};

template <typename T>
class ref
{
public:

	using cb_t = detail::control_block<T>;

	ref() = default;

	ref(cb_t* cb)
		: cb_{cb}
	{
		assert (cb);

		ref_add();
	}

	ref(ref<T>&& rhs) noexcept
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;
		rhs.cb_ = {};
	}

	ref(const ref<T>& rhs)
		: cb_{rhs.cb_}
	{
		if (!cb_) return;

		assert (cb_->ref_count > 0);

		ref_add();
	}

	~ref()
	{
		if (!cb_) return;

		ref_sub();
	}

	auto operator=(ref<T>&& rhs) noexcept -> ref<T>&
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;
		rhs.cb_ = {};

		return *this;
	}

	auto operator=(const ref<T>& rhs) -> ref<T>&
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;

		if (!cb_) return *this;

		assert (cb_->ref_count > 0);

		ref_add();

		return *this;
	}

	auto& get_value() const { return cb_->value; }
	auto operator*() const -> const T& { return cb_->value; }
	auto operator->() const -> const T* { return &cb_->value; }

private:

	auto ref_add() -> void
	{
		assert (cb_);

		cb_->ref_count++;
	}

	auto ref_sub() -> void
	{
		assert (cb_);

		if (cb_->ref_count.fetch_sub(1) == 1)
		{
			delete cb_;
		}
	}

	cb_t* cb_{};

	friend class object<T>;
};

/////////////////////////////////////////////////////////////////////////
/// sync signal /////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

class sync_signal
{
public:

	auto get_value() const { return value_; }
	auto notify() -> void { value_++; }

private:

	uint32_t value_{0};
};

/////////////////////////////////////////////////////////////////////////
/// signal synced object ////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

template <class T, class SignalType = sync_signal>
class signal_synced_object
{
public:

	using me_t = signal_synced_object<T, SignalType>;
	using object_t = object<T>;
	using ref_t = ref<T>;

private:

	struct critical_t
	{
		template <typename... Args>
		critical_t(Args... args) : object{args...} {}

		object_t object;
		std::atomic_bool value_pending{true};
	} critical_;

public:

	template <typename... Args>
	signal_synced_object(const SignalType& signal, Args... args)
		: critical_{args...}
		, read{this, signal}
	{
	}

	struct read_t
	{
		read_t(me_t* self, const SignalType& signal) : self_{self}, signal_{&signal} {}

		auto get_value() -> const T&
		{
			update();

			return current_.get_value();
		}

		auto is_value_pending() const -> bool
		{
			return self_->critical_->value_pending.load();
		}

	private:

		auto update() -> void
		{
			const auto signal_value{signal_->get_value()};

			if (signal_value > slot_value_)
			{
				get_new_value_if_pending();
			}

			slot_value_ = signal_value;
		}

		auto get_new_value_if_pending() -> void
		{
			const auto value_pending{self_->critical_.value_pending.exchange(false)};

			if (value_pending)
			{
				current_ = self_->critical_.object.read.acquire();
			}
		}

		const SignalType* signal_;
		uint32_t slot_value_{0};
		ref_t current_;
		me_t* self_;
	} read;

	struct write_t
	{
		write_t(me_t* self) : self_{self} {}

		auto get_value() -> const T&
		{
			return self_->critical_.object.read.get_value();
		}

		template <typename U>
		auto set(U&& value) -> void
		{
			self_->critical_.object.write.set(std::forward<U>(value));
			self_->critical_.value_pending = true;
		}

		template <typename UpdateFn>
		auto update(UpdateFn&& fn) -> void
		{
			self_->critical_.object.write.update(std::forward<UpdateFn>(fn));
			self_->critical_.value_pending = true;
		}

	private:

		me_t* self_;
	} write{this};
};

/////////////////////////////////////////////////////////////////////////
/// signal synced object pair ///////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

template <class T, class SignalType = sync_signal>
class signal_synced_object_pair
{
public:

	using me_t = signal_synced_object_pair<T, SignalType>;
	using object_t = object<T>;
	using ref_t = ref<T>;

private:

	struct critical_t
	{
		template <typename... Args>
		critical_t(Args... args) : object{args...} {}

		object_t object;
		std::atomic_bool value_pending{true};
	} critical_;

public:

	template <typename... Args>
	signal_synced_object_pair(const SignalType& signal, Args... args)
		: critical_{args...}
		, read{this, signal}
	{
	}

	struct read_t
	{
		read_t(me_t* self, const SignalType& signal) : self_{self}, signal_{&signal} {}

		// Get the value stored in the given cell.
		// If there isn't one yet, fall back to the other cell.
		// If there isn't a value in the other cell either,
		// call update() to get the pending value (there should
		// always be one waiting in this case.)
		auto get_value(int8_t cell) -> const T&
		{
			assert(cell == 0 || cell == 1);

			if (have_value_[cell])
			{
				return current_[cell].get_value();
			}

			if (have_value_[1-cell])
			{
				return current_[1-cell].get_value();
			}

			update(cell);

			assert(have_value_[cell]);

			return current_[cell].get_value();
		}

		auto is_value_pending() const -> bool
		{
			return self_->critical_.value_pending.load();
		}

		// If there is a new value pending, store it in the
		// given cell, but only if we were signalled
		auto update(int8_t cell) -> void
		{
			assert(cell == 0 || cell == 1);

			const auto signal_value{signal_->get_value()};

			if (signal_value > slot_value_)
			{
				get_new_value_if_pending(cell);
			}

			slot_value_ = signal_value;
		}

	private:

		// If there is a new value pending,
		// store it in the given cell
		auto get_new_value_if_pending(int8_t cell) -> void
		{
			assert(cell == 0 || cell == 1);

			const auto value_pending{self_->critical_.value_pending.exchange(false)};

			if (value_pending)
			{
				current_[cell] = self_->critical_.object.read.acquire();
				have_value_[cell] = true;
			}
		}

		const SignalType* signal_;
		uint32_t slot_value_{0};
		std::array<ref_t, 2> current_;
		std::array<bool, 2> have_value_;
		me_t* self_;
	} read;

	struct write_t
	{
		write_t(me_t* self) : self_{self} {}

		template <typename U>
		auto set(U&& value) -> void
		{
			self_->critical_.object.write.set(std::forward<U>(value));
			self_->critical_.value_pending = true;
		}

		template <typename UpdateFn>
		auto update(UpdateFn&& fn) -> void
		{
			self_->critical_.object.write.update(std::forward<UpdateFn>(fn));
			self_->critical_.value_pending = true;
		}

	private:

		me_t* self_;
	} write{this};
};

struct trigger
{
	trigger(std::memory_order memory_order = std::memory_order_relaxed)
		: memory_order_ { memory_order }
	{
		flag_.test_and_set(memory_order);
	}

	auto operator()() -> void
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
class beach_ball
{
public:

	beach_ball(int first_catcher)
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
class beach_ball_player
{
public:

	beach_ball* const ball;

	beach_ball_player(beach_ball* ball_)
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
