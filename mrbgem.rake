MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.authors = File.readlines("#{spec.dir}/AUTHORS").map(&:strip)
  spec.add_dependency 'mruby-errno'
  spec.add_conflict 'mruby-msgpack'

  if build.is_a?(MRuby::CrossBuild)
    msgpackc = "#{spec.dir}/msgpack-c"
    spec.cc.include_paths << "#{msgpackc}/include"
    spec.objs += %W(
      #{msgpackc}/src/unpack.c
      #{msgpackc}/src/version.c
      #{msgpackc}/src/zone.c
    ).map { |f| f.relative_path_from(dir).pathmap("#{build_dir}/%X#{spec.exts.object}" ) }
  elsif spec.cc.search_header_path('msgpack.h') && spec.cc.search_header_path('msgpack/version_master.h')
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
