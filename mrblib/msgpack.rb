module MessagePack
  def self.register_ext_type(type, klass, pack:, unpack:)
    pack = pack.to_proc if pack.respond_to?(:to_proc)
    raise ArgumentError, "packer is no proc" unless pack.is_a?(Proc)
    unpack = unpack.to_proc if unpack.respond_to?(:to_proc)
    raise ArgumentError, "unpacker is no proc" unless unpack.is_a?(Proc)

    register_pack_type(type, klass, &pack)
    register_unpack_type(type, &unpack)

    nil
  end
end
