mruby-simplemsgpack_ext
=======================

Example
-------

```ruby
packed_hash = {foo: "bar", baz: [1, {ret: "val"}]}.to_msgpack + "bye".to_msgpack

MessagePack.unpack(packed_hash) do |result|
  puts result
end
```

By default, MessagePack packs symbols as strings and does not convert them
back when unpacking them. Symbols can be preserved by registering an extension
type for them.

Packing an object is also possible using `MessagePack#pack`
```ruby
MessagePack.pack(:an_object)
````

Extension Types
---------------

To customize how objects are packed, define an extension type:

```ruby
MessagePack.register_pack_type(0, Symbol) { |symbol| symbol.to_s }
MessagePack.register_unpack_type(0) { |data| data.to_sym }

MessagePack.unpack(:symbol.to_msgpack) # => :symbol
```

For nil, true, false, Fixnum, Float, String, Array and Hash a registered
ext type is ignored. They are always packed according to the MessagePack
specification.

Acknowledgements
----------------
This is using code from

- https://github.com/msgpack/msgpack-c
    Copyright (C) 2008-2015 FURUHASHI Sadayuki
    Distributed under the Boost Software License, Version 1.0.
    http://www.boost.org/LICENSE_1_0.txt

- https://github.com/Asmod4n/mruby-simplemsgpack
    Copyright 2015 Hendrik Beskow
    Distributed under the Apache License, Version 2.0
    http://www.apache.org/licenses/LICENSE-2.0
