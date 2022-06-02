# Beach Ball Synchronization
There is a class [in the stupid header](include/stupid/stupid.hpp) named `stupid::BeachBall` which can be used to coordinate exclusive access to some memory between exactly two threads without any locks (with some hardware synchronization through aquire/release `std::atomic` memory ordering.)

This mechanism is intended for situations where both threads are running their own loop and will periodically work on some shared memory.

I don't know if there is already a name for this technique because I don't know anything.

I use this technique in Blockhead to generate mipmap data for a sample buffer in the main (GUI) thread while simultaneously writing data to it in the audio thread.

The beach ball is an analogy -
 - Two threads (two players) want to access some memory
 - Only the player who is currently holding the ball is allowed to access the memory
 - The player who currently has the ball must call `throw_ball()` to throw the ball to the other player
 - Calling `throw_ball()` when you don't have the ball is invalid
 - The other player periodically calls `catch_ball()` which can succeed or fail depending on whether or not the ball has been thrown to them

```c++
// The two players are identified by the IDs 0 or 1. Any other integer is invalid
static constexpr int PLAYER_A { 0 };
static constexpr int PLAYER_B { 1 };
static constexpr int WHO_STARTS_WITH_THE_BALL { PLAYER_A };

stupid::BeachBall ball{ WHO_STARTS_WITH_THE_BALL };
SomeBufferType buffer;
```
```c++
bool player_A_has_the_ball{ false };

auto player_A_process() -> void
{
  if (!player_A_has_the_ball)
  {
    // Try to catch the ball.
    // If this call fails then it means the ball hasn't been thrown to us and is
    // being held by the other player,
    // i.e. the other thread is working on the memory
    if (!ball.catch_ball<PLAYER_A>()) return;
    
    player_A_has_the_ball = true;
  }
  
  // If we got here then we have the ball (we either already had it, or we just
  // now caught it) which means we're allowed to access the memory
  
  player_A_modifies_the_buffer(&buffer);
  
  // We've finished modifying the buffer. Throw the ball back to the other player
  player_A_has_the_ball = false;
  ball.throw_ball<PLAYER_A>();
}
```
```c++
bool player_B_has_the_ball{ false };

auto player_B_process() -> void
{
  if (!player_B_has_the_ball)
  {
    if (!ball.catch_ball<PLAYER_B>()) return;
    
    player_B_has_the_ball = true;
  }
  
  player_B_modifies_the_buffer(&buffer);
  
  player_B_has_the_ball = false;
  ball.throw_ball<PLAYER_B>();
}
```
There is a convenience class, `stupid::BeachBallPlayer` which can keep track of whether or not the player currently has the ball for you. So the above code can be re-written as:
```c++
static constexpr int PLAYER_A { 0 };
static constexpr int PLAYER_B { 1 };
static constexpr int WHO_STARTS_WITH_THE_BALL { PLAYER_A };

stupid::BeachBall ball{ WHO_STARTS_WITH_THE_BALL };
stupid::BeachBallPlayer<PLAYER_A> player_A{ &ball };
stupid::BeachBallPlayer<PLAYER_B> player_B{ &ball };
SomeBufferType buffer;
```
```c++
auto player_A_process() -> void
{
  // If we already have the ball, returns true
  // Otherwise tries to catch the ball
  // Returns false if the ball hasn't been thrown to us yet
  if (!player_A.ensure()) return;
  
  // If we got here then we have the ball (we either already had it, or we just
  // now caught it) which means we're allowed to access the memory
  
  player_A_modifies_the_buffer(&buffer);
  
  // We've finished modifying the buffer. Throw the ball back to the other player
  player_A.throw_ball();
}
```
```c++
auto player_B_process() -> void
{
  if (!player_B.ensure()) return;
  
  player_B_modifies_the_buffer(&buffer);
  
  player_B.throw_ball();
}
```
