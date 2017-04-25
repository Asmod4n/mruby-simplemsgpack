[![Build Status](https://travis-ci.org/Asmod4n/mruby-simplemsgpack.svg?branch=master)](https://travis-ci.org/Asmod4n/mruby-simplemsgpack)

mruby-simplemsgpack
===================

Example
-------

Objects can be packed with `Object#to_msgpack` or `MessagePack.pack`:

```ruby
packed_hash = { a: 'hash', with: [1, 'embedded', 'array'] }.to_msgpack
packed_string = MessagePack.pack('bye')

packed_hash   # => "\x82\xA1a\xA4hash\xA4with\x93\x01\xA8embedded\xA5array"
packed_string # => "\xA3bye"
```

They are unpacked with `MessagePack.unpack`:

```ruby
MessagePack.unpack(packed_hash)   # => { a: 'hash', with: [1, 'embedded', 'array'] }
MessagePack.unpack(packed_string) # => 'bye'
```

A string with multiple packed values can be unpacked by handing a block to
`MessagePack.unpack`:

```ruby
packed = packed_string + packed_hash
unpacked = []
MessagePack.unpack(packed) do |result|
  unpacked << result
end
unpacked # => ['bye', { a: 'hash', with: [1, 'embedded', 'array'] }]
```

When using `MessagePack.unpack` with a block and passing it a incomplete packed Message
it returns the number of bytes it was able to unpack, if it was able to unpack the howl Message it returns self.
This is helpful if the given data contains an incomplete
last object and we want to continue unpacking after we have more data.

```ruby
packed = packed_string + packed_hash.slice(0, packed_hash.length/2)
unpacked = []
unpacked_length = MessagePack.unpack(packed) do |result|
  unpacked << result
end
unpacked_length # => 4 (length of packed_string)
unpacked # => ['bye']
```

Procs, Blocks or Lambdas
========================

mruby allows you to serialize the body of a code block, this wrapper does this via the means of a extension type.
The type number used by default is 127 (the highest number msgpack allows for extension types), you can change it in mrbgem.rake of this gem.
To be able to unpack them you need to compile mruby with ```#define MRB_USE_ETEXT_EDATA```, setable in include/mrbconf.h of your mruby directory.

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

Other objects like classes can also be preserved:

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
