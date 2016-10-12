assert("NilClass#to_msgpack") do
  assert_equal(nil, MessagePack.unpack(nil.to_msgpack))
end

assert("FalseClass#to_msgpack") do
  assert_equal(false, MessagePack.unpack(false.to_msgpack))
end

assert("TrueClass#to_msgpack") do
  assert_equal(true, MessagePack.unpack(true.to_msgpack))
end

assert("Integer#to_msgpack") do
  assert_equal(1, MessagePack.unpack(1.to_msgpack))
  assert_equal(-1, MessagePack.unpack(-1.to_msgpack))
  assert_equal(-2**63, MessagePack.unpack((-2**63).to_msgpack))
  assert_equal(2**64-1, MessagePack.unpack((2**64-1).to_msgpack))
end

assert("Float#to_msgpack") do
  assert_equal(1.2, MessagePack.unpack(1.2.to_msgpack))
  assert_equal(1.7976931348623157e+308, MessagePack.unpack(1.7976931348623157e+308.to_msgpack))
  assert_equal(2.2250738585072014e-308, MessagePack.unpack(2.2250738585072014e-308.to_msgpack))
end

assert("String#to_msgpack") do
  assert_equal('string', MessagePack.unpack('string'.to_msgpack))
end

assert("Symbol#to_msgpack") do
  assert_equal('symbol', MessagePack.unpack(:symbol.to_msgpack))
end

assert("Array#to_msgpack") do
  array = [nil, false, true, 1, -1, 1.2, "string", [], {}]
  assert_equal(array, MessagePack.unpack(array.to_msgpack))
end

assert("Hash#to_msgpack") do
  hash = { nil => nil, false => false, true => true, 1 => 1, 1.2 => 1.2, "string" => "string", [] => [], {} => {} }
  assert_equal(hash, MessagePack.unpack(hash.to_msgpack))
end

assert("Module#to_msgpack") do
  assert_equal('MessagePack::Error', MessagePack.unpack(MessagePack::Error.to_msgpack))
end

assert("MessagePack.register_pack_type") do
  assert_raise(MessagePack::Error, "ext type out of range") do
    MessagePack.register_pack_type(-1, Symbol)
  end

  assert_raise(MessagePack::Error, "ext type out of range") do
    MessagePack.register_pack_type(128, Symbol)
  end

  assert_raise(MessagePack::Error, "no block given") do
    MessagePack.register_pack_type(1, Symbol)
  end

  assert_nil(MessagePack.register_pack_type(1, Symbol) {})
end

assert("MessagePack.register_unpack_type") do
  assert_raise(MessagePack::Error, "ext type out of range") do
    MessagePack.register_unpack_type(-1)
  end

  assert_raise(MessagePack::Error, "ext type out of range") do
    MessagePack.register_unpack_type(128)
  end

  assert_raise(MessagePack::Error, "no block given") do
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

