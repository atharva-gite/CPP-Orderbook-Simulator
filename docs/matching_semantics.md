# Matching semantics

Design decisions for the C++ LOB matching engine. Tests assert these rules;
engine changes that touch icebergs or pro-rata must stay consistent with this note.

## Iceberg replenishment

An iceberg is a limit order with `display_qty > 0`. Only `remaining_qty` (the
current visible slice) participates in matching and in the price-level
aggregate. `hidden_qty` holds the undisplayed remainder; `qty` is the original
total.

When a fill drives `remaining_qty` to zero:

1. If `hidden_qty == 0`, the order is fully done: remove from the level and free.
2. If `hidden_qty > 0`, **replenish**:
   - Peel `slice = min(display_qty, hidden_qty)`.
   - Set `remaining_qty = slice`, `hidden_qty -= slice`.
   - **Lose time priority**: detach from the current position and `push_back` onto
     the same price level (new timestamp / back of FIFO queue).

Consequences:

- After A’s visible slice is exhausted at a level that also holds B, the next
  aggressive fill hits **B first**, then A’s new slice (see
  `iceberg replenishment loses time priority`).
- Within a single aggressive order, matching always consumes the **current
  head**. After replenishment the iceberg sits at the tail, so other resting
  orders at that price trade before it is eligible again; once it returns to
  the head, further slices may trade in the same aggressive order.

`configure_iceberg` must adjust the level aggregate when shrinking visible size
from full `qty` down to `display_qty` on entry; otherwise depth is overstated.

## Pro-rata remainder rounding

At a single price level, for incoming size `Q` and resting visible sizes
`s_i` with `T = sum s_i`:

1. Base allocation: `a_i = floor(s_i * Q / T)`.
2. Leftover units: `L = Q - sum a_i` (clamped at 0).
3. Distribute one unit at a time to orders sorted by:
   - fractional remainder `(s_i * Q) % T` **descending**, then
   - `order_id` **ascending** (deterministic tie-break).
4. Optional `min_fill`: after step 3, any `a_i < min_fill` is zeroed (units are
   not redistributed). Callers that need strict full fill should use
   `min_fill = 1` only when every positive floor share is already ≥ 1, or accept
   shortfall.

Pro-rata uses **visible** `remaining_qty` only (iceberg hidden size does not
participate in the ratio). Fills are applied via `apply_fill`, which uses the
same replenishment rule as FIFO market matching.
