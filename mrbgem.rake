MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'mruby bindings for msgpack'
  spec.add_dependency 'mruby-errno'
  spec.add_conflict 'mruby-msgpack'
end
