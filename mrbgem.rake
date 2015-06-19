MRuby::Gem::Specification.new('mruby-msgpack') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'mruby bindings for msgpack'
  spec.linker.libraries << 'msgpack'
end
