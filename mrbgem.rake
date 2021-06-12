require_relative 'mrblib/version'

MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license = 'Apache-2'
  spec.summary = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.authors = File.readlines("#{spec.dir}/AUTHORS").map(&:strip)
  spec.version = MessagePack::VERSION
  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-error'
  spec.add_conflict 'mruby-msgpack'

  if build.is_a?(MRuby::CrossBuild)  
    unless File.exists?("#{spec.build_dir}/lib/libmsgpackc.a")
      cmake_opts = "-DCMAKE_SYSTEM_NAME=\"#{build.build_target}\" -DCMAKE_HOST_SYSTEM_NAME=\"#{build.host_target}\" -DCMAKE_INSTALL_PREFIX=\"#{spec.build_dir}\" -DCMAKE_CXX_COMPILER=\"#{spec.cxx.command}\" -DCMAKE_CXX_COMPILER_AR=\"#{spec.archiver.command}\" -DCMAKE_CXX_FLAGS=\"#{spec.cxx.flags.join(' ')}\" -DCMAKE_C_COMPILER=\"#{spec.cc.command}\" -DCMAKE_C_COMPILER_AR=\"#{spec.archiver.command}\" -DCMAKE_C_FLAGS=\"#{spec.cc.flags.join(' ')}\" -DCMAKE_LINKER=\"#{spec.linker.command}\" -DCMAKE_MODULE_LINKER_FLAGS=\"#{spec.linker.flags.join(' ')}\""
      sh "mkdir -p #{spec.build_dir}/build && cd #{spec.build_dir}/build && cmake #{cmake_opts} #{spec.dir}/deps/msgpack-c/ && cmake --build . && cmake --build . --target install"
    end
    spec.linker.flags_before_libraries << "\"#{spec.build_dir}/lib/libmsgpackc.a\""
    spec.cc.include_paths << "#{spec.build_dir}/include"
    spec.cxx.include_paths << "#{spec.build_dir}/include"
    build.cc.include_paths << "#{spec.build_dir}/include"
    build.cxx.include_paths << "#{spec.build_dir}/include"
  elsif spec.cc.search_header_path('msgpack.h') && spec.cc.search_header_path('msgpack/version_master.h')
    if spec.build.toolchains.include? 'visualcpp'
      spec.linker.libraries << 'libmsgpackc'
    else
      spec.linker.libraries << 'msgpackc'
    end
  else
    `pkg-config --cflags msgpack 2>/dev/null`.split("\s").each do |cflag|
      spec.cxx.flags << cflag
      spec.cc.flags << cflag
    end
    exitstatus = $?.exitstatus
    `pkg-config --libs msgpack 2>/dev/null`.split("\s").each do |lib|
      spec.linker.flags_before_libraries << lib
    end
    exitstatus += $?.exitstatus
    unless exitstatus == 0
      unless File.exists?("#{spec.build_dir}/lib/libmsgpackc.a")
        warn "mruby-simplemsgpack: cannot find libmsgpackc, building it"
        cmake_opts = "-DCMAKE_INSTALL_PREFIX=\"#{spec.build_dir}\" -DCMAKE_CXX_COMPILER=\"#{spec.cxx.command}\" -DCMAKE_CXX_COMPILER_AR=\"#{spec.archiver.command}\" -DCMAKE_CXX_FLAGS=\"#{spec.cxx.flags.join(' ')}\" -DCMAKE_C_COMPILER=\"#{spec.cc.command}\" -DCMAKE_C_COMPILER_AR=\"#{spec.archiver.command}\" -DCMAKE_C_FLAGS=\"#{spec.cc.flags.join(' ')}\" -DCMAKE_LINKER=\"#{spec.linker.command}\" -DCMAKE_MODULE_LINKER_FLAGS=\"#{spec.linker.flags.join(' ')}\""
        sh "mkdir -p #{spec.build_dir}/build && cd #{spec.build_dir}/build && cmake #{cmake_opts} #{spec.dir}/deps/msgpack-c/ && cmake --build . && cmake --build . --target install"
      end
      spec.linker.flags_before_libraries << "\"#{spec.build_dir}/lib/libmsgpackc.a\""
      spec.cc.include_paths << "#{spec.build_dir}/include"
      spec.cxx.include_paths << "#{spec.build_dir}/include"
      build.cc.include_paths << "#{spec.build_dir}/include"
      build.cxx.include_paths << "#{spec.build_dir}/include"
    end
  end
end
