MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.author  = 'Hendrik Beskow, Christopher Aue'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-string-is-utf8'
  spec.add_conflict 'mruby-msgpack'
  spec.cc.defines << 'MRB_MSGPACK_PROC_EXT=127'

  if spec.cc.search_header_path 'msgpack.h'
    if spec.build.toolchains.include? 'visualcpp'
      spec.linker.libraries << 'libmsgpackc'
    else
      spec.linker.libraries << 'msgpackc'
    end
  else
    msgpackc = "#{spec.dir}#{build.file_separator}msgpack-c"
    spec.cc.include_paths << "#{msgpackc}#{build.file_separator}include"
    spec.objs += %W(
      #{msgpackc}#{build.file_separator}src#{build.file_separator}unpack.c
      #{msgpackc}#{build.file_separator}src#{build.file_separator}version.c
      #{msgpackc}#{build.file_separator}src#{build.file_separator}zone.c
    ).map { |f| f.relative_path_from(dir).pathmap("#{build_dir}#{build.file_separator}%X#{spec.exts.object}" ) }
  end
end
