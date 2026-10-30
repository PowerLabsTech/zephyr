# isotp_runtime_context notes

This is our wrapper around Zephyr's isotp. It is not standard isotp, so read this before touching it.
It lives in the zephyr fork (west_deps/zephyr). `west update` will wipe uncommitted changes.

Used by PaiMoC (one context per subcontroller + one for the Display) and by the subcontroller (one context for PaiMoC).

## Why it exists

Zephyr isotp gives you a send ctx and a recv ctx. Both are on the same pair of CAN IDs (our tx id, their tx id).
Problem: when we send a multi frame message, the peer answers with flow control frames on *its* tx id, which is the same id our recv ctx is bound to. The recv ctx grabs them and the send fails with "unexpected PDU".

So we unbind recv before every send, and bind it again when the send is done. That is what the work handler does.

Consequence: **we cannot listen while we are sending.** Anything the peer sends to us during our send is lost. 

## How a send works

- `isotp_runtime_context_send` just puts the message in a msgq (QUEUE_SIZE deep, 5 in both apps) and returns. It does not wait for delivery.
- One work item per context, so only one send at a time per peer. Before that, two sends could run together and corrupt each other.
- Queue full -> `-11` and the log `Failed to queue message`. The message is gone, caller has to deal with it.

## Retry

If a send fails (no flow control, unexpected PDU, bus off...) the same message is retried.
Delay = `ISOTP_BS_TIMEOUT << retries`, capped at 5 min, plus random jitter. The jitter is so 8 subcontrollers don't all retry at the same moment after a bus hiccup.

So the delays go 0.3 s, 0.7, 1.2, 2.5, 5, 9, 18, 38, 86 s... that matters, see below.

## Receive

`isotp_runtime_context_recv` waits until recv is bound, then calls `isotp_recv`.
When the work handler unbinds recv to send, the waiting recv returns `-14` (ISOTP_RECV_TIMEOUT). It is **not** a real timeout, it is just us cancelling the wait. Callers log it and loop. Ignore it.

## Changes made Oct 2026 and why

Found during the Display restart tests. Display was turned off in the middle of a swap, at pickup.

What happened:
1. The unlock for the pickup cubicle could not be delivered (no flow control). It stayed as the current send and the retry backoff grew to 86 s.
2. The swap kept queuing stuff for the Display (door request, second unlock, screens). Queue filled, everything after got `-11`, including the display details. So when the Display came back it sat on "offline" for ~45 s, because PaiMoC was waiting out its 86 s timer before touching the queue again.
3. When the timer fired, the queue drained in order: unlock cubicle 1, unlock cubicle 1, unlock cubicle 0. Both doors opened for a swap that had ended 2 minutes earlier.

Fixes:

**Send TTL.** Every queued send records `queued_at`. In the work handler, a message older than `ISOTP_RUNTIME_CONTEXT_SEND_TTL_MS` (default 10 s) is dropped, log `Dropping message of len N queued X ms ago`. Dropping resets the backoff and moves on to the next one in the queue.
Reason: nothing we send is still valid after 10 s. An old unlock is dangerous, an old screen gets resent anyway, telemetry has newer data behind it.

**Retry on receive.** In `isotp_runtime_context_recv`, if we get a message from the peer while a retry timer is pending, stop the timer and retry now.
Reason: if the peer is talking to us it is alive, no point waiting out 86 s. The Display's first heartbeat after boot now clears the backlog within a second.

Also `TX completed successfully` went from INF to DBG, it was flooding the console on PaiMoC with 9 peers.

## Config (prj.conf)

- `ISOTP_RUNTIME_CONTEXT_MAX_SEND_LEN` 240 on PaiMoC, 180 on subcontroller. Biggest protobuf we send.
- `ISOTP_RUNTIME_CONTEXT_QUEUE_SIZE` 5.
- `ISOTP_RUNTIME_CONTEXT_SEND_TTL_MS` 10000.
- `ISOTP_BS_TIMEOUT` 250 on PaiMoC, 300 on subcontroller. This is also the first retry delay.

## Known limits

- Can't listen while sending (see above). If this becomes a problem the fix is separate CAN IDs for flow control, which means changing the addressing on all three devices.
- A retry pending on a dead peer still blocks the queue until the TTL drops it. Max wait is TTL + current backoff step.
- No delivery feedback to the caller. If you need to know the message arrived, the peer has to ack at the application level (the Display does this with render acks and unlock results).
