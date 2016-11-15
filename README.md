[![Build Status](https://travis-ci.org/Asmod4n/mruby-simplemsgpack.svg?branch=master)](https://travis-ci.org/Asmod4n/mruby-simplemsgpack)

mruby-simplemsgpack
===================

Example
-------

```ruby
packed_hash = {foo: "bar", baz: [1, {ret: "val"}]}.to_msgpack + "bye".to_msgpack

MessagePack.unpack(packed_hash) do |result|
  puts result
end
```

Packing an object is also possible using `MessagePack#pack`
```ruby
MessagePack.pack(:an_object)
````

Extension Types
---------------

To customize how objects are packed, define an [extension type](https://github.com/msgpack/msgpack/blob/master/spec.md#types-extension-type).

By default, MessagePack packs symbols as strings and does not convert them
back when unpacking them. Symbols can be preserved by registering an extension
type for them:

```ruby
sym_ext_type = 0
MessagePack.register_pack_type(sym_ext_type, Symbol) { |symbol| symbol.to_s }
MessagePack.register_unpack_type(sym_ext_type) { |data| data.to_sym }

MessagePack.unpack(:symbol.to_msgpack) # => :symbol
```

Other object like classes can also be preserved:

```ruby
cls_ext_type = 1
MessagePack.register_pack_type(cls_ext_type, Class) { |cls| cls.to_s }
MessagePack.register_unpack_type(cls_ext_type) { |data| data.constantize }
MessagePack.unpack(Object.to_msgpack) # => Object
````

For nil, true, false, Fixnum, Float, String, Array and Hash a registered
ext type is ignored. They are always packed according to the [MessagePack
specification](https://github.com/msgpack/msgpack/blob/master/spec.md).

Acknowledgements
----------------
This is using code from https://github.com/msgpack/msgpack-c

Copyright (C) 2008-2015 FURUHASHI Sadayuki

   Distributed under the Boost Software License, Version 1.0.
   (See accompanying file LICENSE_1_0.txt or copy at
   http://www.boost.org/LICENSE_1_0.txt)
