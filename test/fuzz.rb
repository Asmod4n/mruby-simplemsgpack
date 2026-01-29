chars = (0..255).map { |i| i.chr }

def random_bytes(len, chars)
  s = ""
  len.times { s << chars[rand(chars.length)] }
  s
end

def random_ruby_value
  case rand(6)
  when 0 then rand(1_000_000)
  when 1 then rand < 0.5
  when 2 then nil
  when 3 then Array.new(rand(10)) { rand(1000) }
  when 4 then { "a" => rand(1000), "b" => rand(1000) }
  when 5 then "x" * rand(50)
  end
end

def random_pointer(chars)
  "/" + Array.new(rand(4)) {
    t = ""
    rand(6).times { t << chars[rand(chars.length)] }
    t
  }.join("/")
end

10_000.times do
  # 1) Byte‑Fuzzing
  begin
    MessagePack.unpack(random_bytes(rand(40), chars))
  rescue MessagePack::Error
  end

  # 2) Roundtrip‑Fuzzing
  begin
    v = random_ruby_value
    MessagePack.unpack(MessagePack.pack(v))
  rescue MessagePack::Error
  end

  # 3) Lazy‑Fuzzing
  begin
    data = MessagePack.pack(random_ruby_value)
    lazy = MessagePack.unpack_lazy(data)
    lazy.value
  rescue MessagePack::Error
  end

  # 4) JSON‑Pointer‑Fuzzing
  begin
    data = MessagePack.pack({ "x" => 1, "y" => [1,2,3], "z" => { "a" => 5 } })
    lazy = MessagePack.unpack_lazy(data)
    lazy.at_pointer(random_pointer(chars))
  rescue MessagePack::Error, KeyError, IndexError, TypeError
  end

  # 5) Ext‑Type‑Fuzzing
  begin
    MessagePack.register_ext_type(42, String,
      pack: ->(s) { s },
      unpack: ->(d) { d }
    )
    MessagePack.unpack(random_bytes(rand(20), chars))
  rescue MessagePack::Error
  end
end
