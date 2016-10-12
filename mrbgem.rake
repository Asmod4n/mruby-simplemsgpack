MRuby::Gem::Specification.new('mruby-simplemsgpack_ext') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow, Christopher Aue'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = "https://github.com/christopheraue/mruby-simplemsgpack_ext"
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-string-is-utf8'
  spec.add_conflict 'mruby-msgpack'
  spec.add_conflict 'mruby-simplemsgpack'
end
