# stupid
Header-only lock-free synchronization utilities (one writer, many readers)

## Base functionality

The base functionality of this library is provided by the classes:
* `stupid::Object<T>`
* `stupid::Immutable<T>`

### Very basic usage example

```c++
stupid::Object<Thing> thing;
```

`In the writer thread`
```c++
// Initialize the object
thing.write().commit_new();

// That's equivalent to doing this
thing.write().commit(new Thing{});

// Create a copy of the object to modify
auto copy = thing.write().copy();

copy->modify();
copy->the();
copy->object();

// Commit the modified object
thing.write().commit(copy);
```
`In the reader thread`
```c++
// Get a reference to the most recently committed version of the object
stupid::Immutable<Thing> ref = thing.read().get();

ref->access();
ref->read_only();
ref->stuff();

// stupid::Immutable is a reference counted type. The referenced version of
// the object will remain valid as long as there are references to it, even
// if the writer thread commits new versions of the object.

```

### Caveats
* Only one simultaneous writer thread is supported
* All `stupid::Immutable`'s for a `stupid::Object` must be deleted before the `stupid::Object` is destructed

### Notes
* Only these methods allocate memory:
    - `stupid::Write::commit_new()`
    - `stupid::Write::copy()`

* Only these methods deallocate memory (in the form of garbage collection of old versions of the object):
    - `stupid::Object::~Object()`
    - `stupid::Write::commit()`
    - `stupid::Write::commit_new()`
 
* Memory is deallocated using `delete`

## Additional classes

Some additional, higher-level classes are provided for more specific use cases:
* `stupid::SyncSignal`
* `stupid::SignalSyncObject<T>`
* `stupid::SignalSyncObjectPair<T>`
* `stupid::QuickSync<T>`

### Possible `SignalSyncObject` usage in an audio application

The audio callback in this example has the following stipulations:
 - May not allocate or deallocate memory
 - May not lock a mutex
 - Should be able to get a reference to some immutable data by calling some function e.g. `get_data()`
 - Repeated calls to `get_data()` within the same invocation of the audio callback must return the same reference
 - The data returned from `get_data()` must remain valid for the duration of the current invocation of the audio callback

```c++
struct
{
	stupid::SyncSignal signal;
	stupid::SignalSyncObject<AudioData> data;
	
	Sync() : data(signal) {}
} sync;
```

`UI thread`
```c+++
void update_audio_data()
{
	auto copy = sync.data.copy();

	copy->modify();
	copy->the();
	copy->data();
	
	sync.data.commit(copy);
}
```
`Audio thread`
```c++
void audio_callback(...)
{
	// stupid::SyncSignal::operator() is called once at the start
	// of each audio buffer, and nowhere else.

	// This increments its value by 1.

	// The signal's value is checked whenever
	// stupid::SignalSyncObject::get_data() is called.
	sync.signal();

	...

	// stupid::SignalSyncObject::get_data() is guaranteed to always
	// return a reference to the same data unless:
	//  1. there is new data available, AND
	//  2. the current signal value is greater than the previous
	//     call to get_data().

	// Therefore new data (if there is any) is only retrieved on
	// the first call to get_data() per audio buffer.
	const AudioData& data1 = sync.data.get_data();
	
	data1.use();
	data1.the();
	data1.data();

	/**** UI thread could write new data here, for example ****/
	
	const auto& data2 = sync.data.get_data();
	
	// Will always pass. If new data was written by the UI thread
	// then it won't be picked up until the next audio buffer.
	assert(&data1 == &data2);
}

```
