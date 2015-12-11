assert("Object#to_msgpack") do
  obj = [1, -1, MessagePack, "hallo", nil, false, true, [1, -1, nil, false, true], hashie: {ary: [1, -1, nil, false, true], hashie: {ary: [1, -1, nil, false, true]}}]
  assert_equal(obj, MessagePack.unpack(obj.to_msgpack))
end

