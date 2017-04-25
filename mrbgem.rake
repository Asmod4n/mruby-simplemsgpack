MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow, Christopher Aue'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-string-is-utf8'
  spec.add_conflict 'mruby-msgpack'
  spec.cc.defines << 'MRB_MSGPACK_PROC_EXT=127'
end
