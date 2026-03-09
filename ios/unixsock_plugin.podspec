#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint unixsock_plugin.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'unixsock_plugin'
  s.version          = '0.0.1'
  s.summary          = 'An iOS Unix domain socket Flutter FFI plugin.'
  s.description      = <<-DESC
Unix domain sockets for Flutter on iOS, implemented with Objective-C/C and kqueue.
                       DESC
  s.homepage         = 'https://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Clark' => 'clark@example.com' }
  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*.{h,c,m}'
  s.public_header_files = 'Classes/include/**/*.h'
  s.private_header_files = 'Classes/internal/**/*.h'
  s.dependency 'Flutter'
  s.platform = :ios, '12.0'
  s.static_framework = true
  s.frameworks = 'Foundation'
  s.resource_bundles = { 'unixsock_plugin_privacy' => ['Resources/PrivacyInfo.xcprivacy'] }

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'CLANG_CXX_LANGUAGE_STANDARD' => 'gnu++17'
  }
  s.user_target_xcconfig = { 'OTHER_LDFLAGS' => '$(inherited) -ObjC' }
end
