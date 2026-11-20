# fwd/fwd-await-field-not-found

FWD{ op=AWAIT, dst=/sensor/temp, FIELD :stats.mem.control, src=/reply-ep }: an `AWAIT` with a
`FIELD` selector.

```
0f404100010001000206000c000673656e736f720474656d7010401b00020005007374617473020003006d656d02000700636f6e74726f6c06000900087265706c792d6570
```

```
0F 40 41 00                          FWD, PL = 1, length 65
   01 00 01 00 02                       VALUE op = AWAIT
   06 00 0C 00 06 "sensor" 04 "temp"    PATH dst = /sensor/temp
   10 40 1B 00                          FIELD, PL = 1, length 27
      02 00 05 00 "stats"                  NAME "stats"
      02 00 03 00 "mem"                    NAME "mem"
      02 00 07 00 "control"                NAME "control"
   06 00 09 00 08 "reply-ep"            PATH src = /reply-ep
```

69 bytes total.

## The rule this pins

annex 05 §`:stats`: the seam census is readable and never awaitable, and so is every other field.
Counters are not writes and wake no awaiter, so no wait on a field could ever fire. The terminus
therefore answers any `AWAIT` that carries a `FIELD` with `ERROR{tr::schema::not_found}`
(`0x0031`) at once. It does not wait, it does not wait on the vertex instead, and it does not
answer `tr::flow::timeout`. A field that READ and WRITE serve (`:subscribers`) gets the same
answer.

The frame carries no `await_timeout`. The refusal comes before any wait, so it needs none.

## Bound by

C++ `core/tests/op_resolve_test.cpp`, `test_await_field_selector_is_enotty`, which feeds these
bytes to the resolver and checks for `kind = ERROR` with code `0x0031`, beside a field-less
`AWAIT` control.
