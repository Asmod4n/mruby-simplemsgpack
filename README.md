mruby-simplemsgpack
===================

Breaking changes
================

Starting with Release 2.0 only mruby-3 is supported, if you are on an older version check out a commit from before 2021.

Installation
============

First get a working copy of [mruby](https://github.com/mruby/mruby) then add

```ruby
  conf.gem mgem: 'mruby-simplemsgpack'
```

to the build_conf.rb of the mruby directory

mruby-simplemsgpack searches for msgpack-c on your system, if it can find it it links against it, otherwise it builds against msgpack-c from source.
You need at least msgpack-c 1 and depending on your system also pkg-config.

For building from source you need to have cmake installed on your system, take a look at <https://github.com/msgpack/msgpack-c/blob/c_master/QUICKSTART-C.md#install-with-source-code> for more information.

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
it returns the position of the first offending byte, if it was able to unpack the whole Message it returns self.
This is helpful if the given data contains an incomplete
last object and we want to continue unpacking after we have more data.

```ruby
packed = packed_string + packed_hash.slice(0, packed_hash.length/2)
unpacked = []
offending_byte = MessagePack.unpack(packed) do |result|
  unpacked << result
end
offending_byte # => 19 (length of packed)
unpacked # => ['bye']
```

# Lazy unpacking

Need to pull just a few values from a large MessagePack payload?
`MessagePack.unpack_lazy` can be up to **10× faster** because it avoids fully unpacking the structure — only the parts you ask for are decoded.
It returns a lightweight handle that lets you navigate using JSON Pointers:

```ruby
data = [
  { "id" => 1, "name" => "Alpha" },
  { "id" => 2, "name" => "Beta" },
  { "id" => 3, "name" => "Gamma" },
  { "id" => 4, "name" => "Delta", "meta" => { "active" => true } }
]

packed = MessagePack.pack(data)

lazy = MessagePack.unpack_lazy(packed)

# Access an element by JSON Pointer without fully unpacking everything
lazy.at_pointer("/3/name")        # => "Delta"
lazy.at_pointer("/3/meta/active") # => true
lazy.at_pointer("/3")             # => { "id" => 4, "name" => "Delta", "meta" => { "active" => true } }

# Root access (returns entire unpacked object)
lazy.value  # => full data
```
## Error handling

When using `MessagePack.unpack_lazy(...).at_pointer(pointer)`, specific exceptions are raised for invalid pointers or traversal mistakes:

```ruby
data = [
  { "id" => 1, "name" => "Alpha" },
  { "id" => 2, "name" => "Beta" }
]

lazy = MessagePack.unpack_lazy(MessagePack.pack(data))

# Non-existent array index
lazy.at_pointer("/99/name")
# => IndexError (array index out of range)

# Non-existent key in an object
lazy.at_pointer("/0/nope")
# => KeyError (key not found)

# Attempting to navigate into a scalar value
lazy.at_pointer("/0/name/foo")
# => TypeError (cannot navigate into a string)
```

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
```

For nil, true, false, Integer, Float, String, Array and Hash a registered
ext type is ignored. They are always packed according to the [MessagePack
specification](https://github.com/msgpack/msgpack/blob/master/spec.md).

Proc, blocks or lambas
-----------------------

If you want to pack and unpack mruby blocks take a look at the [mruby-proc-irep-ext](https://github.com/Asmod4n/mruby-proc-irep-ext) gem, it can be registered like the other extension types

Overriding `to_msgpack`
---------------------

It's not supported to override `to_msgpack`, `MessagePack.pack` ignores it, same when that object is included in a Hash or Array.
This gem treats objects like ruby does, if you want to change the way your custom Class gets handled you can add `to_hash`, `to_ary`, `to_int` or `to_str` methods so it will be packed like a Hash, Array, Integer or String (in that order) then.

Acknowledgements
----------------

This is using code from <https://github.com/msgpack/msgpack-c>

Copyright (C) 2008-2015 FURUHASHI Sadayuki

   Distributed under the Boost Software License, Version 1.0.
   (See accompanying file LICENSE_1_0.txt or copy at
   http://www.boost.org/LICENSE_1_0.txt)
