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

