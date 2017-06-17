assert("NilClass#to_msgpack") do
  assert_equal(nil, MessagePack.unpack(nil.to_msgpack))
  assert_equal(nil, MessagePack.unpack(MessagePack.pack(nil)))
end

assert("FalseClass#to_msgpack") do
  assert_equal(false, MessagePack.unpack(false.to_msgpack))
  assert_equal(false, MessagePack.unpack(MessagePack.pack(false)))
end

assert("TrueClass#to_msgpack") do
  assert_equal(true, MessagePack.unpack(true.to_msgpack))
  assert_equal(true, MessagePack.unpack(MessagePack.pack(true)))
end

assert("Integer#to_msgpack") do
  assert_equal(1, MessagePack.unpack(1.to_msgpack))
  assert_equal(1, MessagePack.unpack(MessagePack.pack(1)))
  assert_equal(-1, MessagePack.unpack(-1.to_msgpack))
  assert_equal(-2**63, MessagePack.unpack((-2**63).to_msgpack))
  assert_equal(2**64-1, MessagePack.unpack((2**64-1).to_msgpack))
end

assert("Float#to_msgpack") do
  assert_equal(1.2, MessagePack.unpack(1.2.to_msgpack))
  assert_equal(1.2, MessagePack.unpack(MessagePack.pack(1.2)))
  assert_equal(1.7976931348623157e+308, MessagePack.unpack(1.7976931348623157e+308.to_msgpack))
  assert_equal(2.2250738585072014e-308, MessagePack.unpack(2.2250738585072014e-308.to_msgpack))
end

assert("String#to_msgpack") do
  assert_equal('string', MessagePack.unpack('string'.to_msgpack))
  assert_equal('string', MessagePack.unpack(MessagePack.pack('string')))
  assert_equal("😎", MessagePack.unpack("😎".to_msgpack))
end

assert("Symbol#to_msgpack") do
  assert_equal('symbol', MessagePack.unpack(:symbol.to_msgpack))
  assert_equal('symbol', MessagePack.unpack(MessagePack.pack(:symbol)))
end

assert("Array#to_msgpack") do
  array = [nil, false, true, 1, -1, 1.2, "string", [], {}]
  assert_equal(array, MessagePack.unpack(array.to_msgpack))
  assert_equal(array, MessagePack.unpack(MessagePack.pack(array)))
end

assert("Hash#to_msgpack") do
  hash = { nil => nil, false => false, true => true, 1 => 1, 1.2 => 1.2, "string" => "string", [] => [], {} => {} }
  assert_equal(hash, MessagePack.unpack(hash.to_msgpack))
  assert_equal(hash, MessagePack.unpack(MessagePack.pack(hash)))
end

assert("Module#to_msgpack") do
  assert_equal('MessagePack::Error', MessagePack.unpack(MessagePack::Error.to_msgpack))
end

assert("MessagePack.unpack with block") do
  value1 = 'the first string'
  value2 = 'and this is another one'
  value3 = 'all good things come in threes'

  packed1 = value1.to_msgpack
  packed2 = value2.to_msgpack
  packed3 = value3.to_msgpack

  packed = packed1 + packed2 + packed3
  chunk1 = packed[0..packed1.length+5]
  chunk2 = packed[packed1.length+5..-1]

  unpacked = []

  # unpack everything possible from chunk1
  unpacked_length = MessagePack.unpack(chunk1) { |value| unpacked << value }

  # only value1 can be unpacked
  assert_equal(unpacked_length, packed1.length)
  assert_equal(unpacked, [value1])

  # remove unpacked bytes from chunk1
  slice_start = unpacked_length
  slice_length = chunk1.length - (unpacked_length+1)
  chunk1 = chunk1.slice(slice_start, slice_length)

  # continue unpacking with rest of chunk1 and chunk2
  unpacked_length = MessagePack.unpack(chunk1 + chunk2) { |value| unpacked << value }

  # now, everything is unpacked
  assert_equal(unpacked_length, chunk1.length + chunk2.length)
  assert_equal(unpacked, [value1, value2, value3])
end

assert("MessagePack.register_pack_type") do
  assert_raise(RangeError, "ext type out of range") do
    MessagePack.register_pack_type(-1, Symbol)
  end

  assert_raise(RangeError, "ext type out of range") do
    MessagePack.register_pack_type(128, Symbol)
  end

  assert_raise(ArgumentError, "no block given") do
    MessagePack.register_pack_type(1, Symbol)
  end

  assert_nil(MessagePack.register_pack_type(1, Symbol) {})
end

assert("MessagePack.register_unpack_type") do
  assert_raise(RangeError, "ext type out of range") do
    MessagePack.register_unpack_type(-1)
  end

  assert_raise(RangeError, "ext type out of range") do
    MessagePack.register_unpack_type(128)
  end

  assert_raise(ArgumentError, "no block given") do
    MessagePack.register_unpack_type(1)
  end

  assert_nil(MessagePack.register_unpack_type(1) {})
end

assert("Symbol#to_msgpack with registered ext type") do
  MessagePack.register_pack_type(0, Symbol) { |symbol| symbol.to_s }
  MessagePack.register_unpack_type(0) { |data| data.to_sym }
  assert_equal(:symbol, MessagePack.unpack(:symbol.to_msgpack))

  hash = { key: 123, nested: [:array] }
  assert_equal(hash, MessagePack.unpack(hash.to_msgpack))
  assert_equal(hash, MessagePack.unpack(MessagePack.pack(hash)))
end

assert("Class#to_msgpack with registered ext type") do
  MessagePack.register_pack_type(0, Class) { |mod| mod.to_s }
  MessagePack.register_unpack_type(0) { |data| data.constantize }
  assert_equal(MessagePack::Error, MessagePack.unpack(MessagePack::Error.to_msgpack))
end

assert("Registered ext type for one of the core types is ignored") do
  MessagePack.register_pack_type(0, Array) { |array| nil }
  MessagePack.register_unpack_type(0) { |data| nil }
  assert_equal(['item'], MessagePack.unpack(['item'].to_msgpack))
end

assert("Unknown Ext Type raises a Exception when tried to unpack") do
  assert_raise(MessagePack::Error) do
    MessagePack.register_pack_type(50, Symbol) { |symbol| symbol.to_s }
    MessagePack.unpack(:hallo.to_msgpack)
  end
end

assert("Extension types are inherited") do
  class Test
    def initialize(id)
      @id = id
    end

    attr_reader :id

    def ==(other)
      self.class == other.class and @id == id
    end
  end

  class InheritsTest < Test; end

  MessagePack.register_pack_type(0, Test) { |test| test.class.to_s + '#' + test.id }
  MessagePack.register_unpack_type(0) do |data|
    class_name, id = data.split('#')
    class_name.constantize.new(id)
  end

  assert_equal(Test.new('test'), MessagePack.unpack(Test.new('test').to_msgpack))
  assert_equal(InheritsTest.new('inherited'), MessagePack.unpack(InheritsTest.new('inherited').to_msgpack))
end

assert("Extension types for modules") do
  module Mod; end
  MessagePack.register_pack_type(0, Mod) { |obj| 'packed' }

  class Cls; include Mod end
  assert_equal(Cls.new.to_msgpack, "\xc7\x06\x00packed")

  assert_equal(Object.new.extend(Mod).to_msgpack, "\xc7\x06\x00packed")
end

