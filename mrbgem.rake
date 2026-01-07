require 'fileutils'
require_relative 'mrblib/version'

MRuby::Gem::Specification.new('mruby-simplemsgpack') do |spec|
  spec.license  = 'Apache-2'
  spec.summary  = 'msgpack for mruby including extension types'
  spec.homepage = 'https://github.com/Asmod4n/mruby-simplemsgpack'
  spec.authors  = File.readlines(File.join(spec.dir, 'AUTHORS')).map(&:strip)
  spec.version  = MessagePack::VERSION

  spec.add_dependency 'mruby-errno'
  spec.add_dependency 'mruby-error'
  spec.add_dependency 'mruby-string-is-utf8'
  spec.add_dependency 'mruby-c-ext-helpers'
  spec.add_conflict   'mruby-msgpack'
  spec.cxx.flags << '-std=c++17' if spec.cxx.flags && !spec.cxx.flags.include?('-std=c++17')

  include_dir = File.join(spec.build_dir, 'include')

  unless File.exist?(File.join(include_dir, 'msgpack.hpp'))
    warn 'mruby-simplemsgpack: cannot find msgpack-cxx, building it'

    cmake_opts = "-DMSGPACK_USE_BOOST=OFF -DMSGPACK_INSTALL=ON " \
                 "-DCMAKE_INSTALL_PREFIX=\"#{spec.build_dir}\" " \
                 "-DCMAKE_CXX_COMPILER=\"#{spec.cxx.command}\" " \
                 "-DCMAKE_CXX_COMPILER_AR=\"#{spec.archiver.command}\" " \
                 "-DCMAKE_CXX_FLAGS=\"#{spec.cxx.flags.join(' ')}\" " \
                 "-DCMAKE_C_COMPILER=\"#{spec.cc.command}\" " \
                 "-DCMAKE_C_COMPILER_AR=\"#{spec.archiver.command}\" " \
                 "-DCMAKE_C_FLAGS=\"#{spec.cc.flags.join(' ')}\" " \
                 "-DCMAKE_LINKER=\"#{spec.linker.command}\" " \
                 "-DCMAKE_MODULE_LINKER_FLAGS=\"#{spec.linker.flags.join(' ')}\""

    if build.is_a?(MRuby::CrossBuild)
      cmake_opts << " -DCMAKE_SYSTEM_NAME=\"#{build.build_target}\"" \
                    " -DCMAKE_HOST_SYSTEM_NAME=\"#{build.host_target}\""
    end

    build_dir = File.join(spec.build_dir, 'build')
    FileUtils.mkdir_p(build_dir)
    Dir.chdir(build_dir) do
      sh "cmake #{cmake_opts} #{File.join(spec.dir, 'deps', 'msgpack-c')}"
      sh "cmake --build . --target install"
    end
  end
  dst = File.join(File.expand_path(File.dirname(__FILE__)), 'include')

  FileUtils.mkdir_p(dst)
  FileUtils.cp_r("#{include_dir}/.", dst)

  [spec.cc, spec.cxx].each { |c| c.include_paths << include_dir }
end
