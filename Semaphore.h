/**
 * @file
 * @brief Implementation of Semaphore.
 *
 * Copyright (C) 2019 Carlo Wood.
 *
 * RSA-1024 0x624ACAD5 1997-01-26                    Sign & Encrypt
 * Fingerprint16 = 32 EC A7 B6 AC DB 65 A6  F6 F6 55 DD 1C DC FF 61
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "threadsafe/Futex.h"
#include "debug.h"

namespace aithreadsafe {

// class Semaphore
//
// Represents a token counter that can be added to and taken from.
// Threads that try to take a token that isn't there will block.
//
// The underlaying token counter is stored in the 32bit Futex word
// and is therefore unsigned. The other 32bit represent the number
// of waiting threads.
//
// Because of spurious wake-ups and race conditions, a thread that
// wakes up cannot know if it really had to wake up and must simply
// try to grab a token or go to sleep again.
//
class Semaphore : public Futex<uint64_t>
{
 public:
  Semaphore(uint32_t tokens) : Futex<uint64_t>(tokens) { }

  // The 64 bit of the atomic Futex<uint64_t>::m_word have the following meaning:
  //
  // |___________________________64_bit_word___________________________|
  // |____32 most significant bits____|____32 least significant bits___|
  //  [   number of blocked threads   ][   number of avaiable tokens  ]
  //                                   <--------nwaiters_shift-------->  = 32
  //  00000000000000000000000000000001 00000000000000000000000000000000  = 0x100000000 = one_waiter
  //  00000000000000000000000000000000 11111111111111111111111111111111  =  0xffffffff = tokens_mask
  static constexpr int nwaiters_shift = 32;
  static constexpr uint64_t one_waiter = (uint64_t)1 << nwaiters_shift;
  static constexpr uint64_t tokens_mask = one_waiter - 1;

  // Add n tokens to the semaphore.
  //
  // If there are waiting threads then (at most) n threads are woken up,
  // but with a single system call; each of which then tries to grab one
  // of the available tokens.
  void post(uint32_t n = 1) noexcept
  {
    DoutEntering(dc::notice, "Semaphore::post(" << n << ")");
    // Add n tokens.
    uint64_t prev_word = m_word.fetch_add(n, std::memory_order_relaxed);
#if CW_DEBUG
    uint64_t prev_tokens = prev_word & tokens_mask;
    Dout(dc::notice, "tokens " << prev_tokens << " --> " << (prev_tokens + n));
    // Check for possible overflow.
    ASSERT(prev_tokens + n <= tokens_mask);
#endif
    uint32_t nwaiters = prev_word >> nwaiters_shift;
    // Are there potential waiters that need to be woken up?
    if (nwaiters > 0)
    {
      Dout(dc::notice, "Calling Futex<uint64_t>::wake(" << n << ") because there were waiters (" << nwaiters << ").");
      DEBUG_ONLY(uint32_t woken_up =) Futex<uint64_t>::wake(n);
      Dout(dc::notice, "Woke up " << woken_up << " threads.");
      ASSERT(woken_up <= n);
    }
  }

  // Same as wait() but should only be called when (at least, very recently)
  // there are no tokens so that we are very likely to go to sleep.
  void slow_wait() noexcept;

  bool fast_try_wait() noexcept
  {
    uint64_t word = m_word.load(std::memory_order_relaxed);
    do
    {
      uint64_t ntokens = word & tokens_mask;
      Dout(dc::notice, "tokens = " << ntokens << "; waiters = " << (word >> nwaiters_shift));
      // Are there any tokens to grab?
      if (ntokens == 0)
        return false;           // No debug output needed: if the above line prints tokens = 0 then return false is implied.
      // We seem to have a token, try to grab it.
    }
    while (!m_word.compare_exchange_weak(word, word - 1, std::memory_order_acquire));
    // Token successfully grabbed.
    Dout(dc::notice, "Success, now " << ((word & tokens_mask) - 1) << " tokens left.");
    return true;
  }

  // Removes one token from the semaphore.
  //
  // If no token is available then the thread will block until it manages
  // to grab a new token added with post(n).
  void wait() noexcept
  {
    DoutEntering(dc::notice, "Semaphore::wait()");
    if (!fast_try_wait())
      slow_wait();
  }

  bool try_wait() noexcept
  {
    DoutEntering(dc::notice, "Semaphore::try_wait()");
    return fast_try_wait();
  }
};

} // namespace aithreadsafe