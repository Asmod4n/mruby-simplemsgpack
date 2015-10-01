assert("Object#to_msgpack") do
  assert_equal({foo: "a", bar: ["a", "b", "c"]}.to_msgpack, "\202\307\003\001foo\241a\307\003\001bar\223\241a\241b\241c")
end

assert("MessagePack#unpack") do
  assert_equal(MessagePack.unpack("\202\307\003\001foo\241a\307\003\001bar\223\241a\241b\241c"), {foo: "a", bar: ["a", "b", "c"]})
end
