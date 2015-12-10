[![Build Status](https://travis-ci.org/Asmod4n/mruby-simplemsgpack.svg?branch=master)](https://travis-ci.org/Asmod4n/mruby-simplemsgpack)
# mruby-simplemsgpack

Example
=======

```ruby
packed_hash = {foo: "bar", baz: [1, {ret: "val"}]}.to_msgpack + "bye".to_msgpack

MessagePack.unpack(packed_hash) do |result|
  puts result
end
```

Ruby Symbols
------------

Normally MessagePack knows nothing about Ruby Symbols, but since version 1 the Protocol is extensible.
The usal catch here is that Ruby doesn't Garbage Collect Symbols and would then be able to exhause the Resources of the System when a Attacker would generate a unbounded Number of them.
But mruby has a special function which only returns Symbols which are already created and nil otherwise.

Acknowledgements
----------------
This is using code from https://github.com/msgpack/msgpack-c

Copyright (C) 2008-2015 FURUHASHI Sadayuki

   Distributed under the Boost Software License, Version 1.0.
   (See accompanying file LICENSE_1_0.txt or copy at
   http://www.boost.org/LICENSE_1_0.txt)
