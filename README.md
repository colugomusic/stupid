# stupid
Header-only lock-free synchronization utilities (one writer, many readers). No queues

## Base functionality

The base functionality of this library is provided by the classes:
* `stupid::object<T>`
* `stupid::ref<T>`

### Very basic usage example

```c++
stupid::object<Thing> thing{constructor, args, ...};
```

`In the writer thread`
```c++
thing.write.update([](Thing thing)
{
	thing.modify();
	thing.the();
	thing.object();
	
	return thing;
});
```
`In the reader thread`
```c++
// Get a reference to the most recent version of the object
stupid::ref<Thing> ref = thing.read.acquire();

ref->access();
ref->read_only();
ref->stuff();

// stupid::ref is a reference counted type. The referenced version of
// the object will remain valid as long as there are references to it, even
// if the writer thread commits new versions of the object.

```

### Caveats
* Multiple simultaneous writer threads are not supported

### Notes
* Only these methods allocate memory:
    - `stupid::object::object(...)` (the constructor)
    - `stupid::write_t::update()`

* Only these methods deallocate memory (in the form of garbage collection of old versions of the object):
    - `stupid::object::~object()`
    - `stupid::write_t::update()`
    
* When a `stupid::object` is destroyed, if you are still holding on to any associated `stupid::ref`s then the last one to be destroyed will also deallocate in the destructor, so if you don't want your reader thread to deallocate then make sure you destroy all your `stupid::ref`s before destroying the associated `stupid::object`.

## Additional classes

Some additional, higher-level classes are provided for more specific use cases:
* `stupid::sync_signal`
* `stupid::signal_synced_object<T>`
* `stupid::signal_synced_object_pair<T>`

### Possible `signal_synced_object` usage in an audio application

The audio callback in this example has the following stipulations:
 - May not allocate or deallocate memory
 - May not lock a mutex
 - Should be able to get a reference to some immutable value by calling some function e.g. `get_value()`
 - Repeated calls to `get_value()` within the same invocation of the audio callback must return the same reference
 - The reference returned from `get_value()` must remain valid for the duration of the current invocation of the audio callback

```c++
struct
{
	stupid::sync_signal signal;
	stupid::signal_synced_object<AudioData> data;
	
	Sync() : data(signal) {}
} sync;
```

`UI thread`
```c++
void update_audio_data()
{
	sync.data.write.update([](AudioData data)
	{
		data.modify();
		data.the();
		data.data();
		
		return data;
	});
}
```
`Audio thread`
```c++
void audio_callback(...)
{
	// stupid::sync_signal::notify() is called once at the start
	// of each audio buffer, and nowhere else.

	// This increments its value by 1.

	// The signal's value is checked whenever
	// stupid::signal_synced_object::read_t::get_value() is called.
	sync.signal.notify();

	...

	// stupid::signal_synced_object::read_t::get_value() is guaranteed to always
	// return a reference to the same data unless:
	//  1. there is new data available, AND
	//  2. the current signal value is greater than the previous
	//     call to get_value().

	// Therefore new data (if there is any) is only retrieved on
	// the first call to get_value() per audio buffer.
	const AudioData& data1 = sync.data.read.get_value();
	
	data1.use();
	data1.the();
	data1.data();

	/**** UI thread could write new data here, for example ****/
	
	const auto& data2 = sync.data.read.get_value();
	
	// Will always pass. If new data was written by the UI thread
	// then it won't be picked up until the next audio buffer.
	assert(&data1 == &data2);
}

```
## More Stuff
### stupid::trigger
It's a tiny wrapper around `std::atomic_flag`.

- `stupid::trigger::operator()` primes the trigger
- `stupid::trigger::operator bool` returns true if the trigger was primed, and resets it

#### Example usage
```c++
struct
{
	stupid::trigger start_playback;
	stupid::trigger stop_playback;
} sync;
```
`UI thread`
```c++
void process_ui_events()
{
	if (start_button_pressed)
	{
		sync.start_playback(); // Tell the audio thread to start playback ASAP
	}
	
	if (stop_button_pressed)
	{
		sync.stop_playback(); // Tell the audio thread to stop playback ASAP
	}
}
```
`Audio thread`
```c++
void audio_process()
{
	switch (current_state)
	{
		case Playing:
		{
			if (sync.stop_playback) stop();
			break;
		}
		
		case Stopped:
		{
			if (sync.start_playback) start();
			break;
		}
	}
}
```
### stupid::beach_ball and stupid::beach_ball_player
Can be used to synchronize access to some memory between exactly two threads.

Some documentation here: [beach_ball.md](beach_ball.md)
