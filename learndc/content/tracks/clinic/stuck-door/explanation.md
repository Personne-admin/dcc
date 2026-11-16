# Root cause: The door that locks while open

`next` is a total function from (state, event) to a state. The bug is the pair (Open, Lock): the arm moves the door to Locked instead of leaving it Open. The Closed block has the same arm and there it is correct, a typical copy and paste slip.

The method for state machines: write the transition table on paper, run the event script and compare state by state. Stop at the first difference; later differences are usually consequences (after the wrong lock, the push no longer closes the door, so the rest of the trace drifts).

Keeping the state change in one pure function like `next` is what makes this easy: you can test one pair at a time without a door.
