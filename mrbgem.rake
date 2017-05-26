MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow, Christopher Aue'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-string-is-utf8'
  spec.add_conflict 'mruby-msgpack'
  spec.cc.defines << 'MRB_MSGPACK_PROC_EXT=127'

  if spec.cc.search_header_path('msgpack.h') && spec.cc.search_header_path('msgpack/version_master.h')
    if spec.build.toolchains.include? 'visualcpp'
      spec.linker.libraries << 'libmsgpackc'
    else
      spec.linker.libraries << 'msgpackc'
    end
  else
    msgpackc = "#{spec.dir}/msgpack-c"
    spec.cc.include_paths << "#{msgpackc}/include"
    spec.objs += %W(
      #{msgpackc}/src/unpack.c
      #{msgpackc}/src/version.c
      #{msgpackc}/src/zone.c
    ).map { |f| f.relative_path_from(dir).pathmap("#{build_dir}/%X#{spec.exts.object}" ) }
  end
end
