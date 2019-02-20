import SomeModule

print(SomeFunc())
let wrapper = XWrapper(x: 43)
print(wrapper.x)

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/modulecache)
// RUN: %empty-directory(%t/idx)
//
// Setup builds a parseable interface for a module SomeModule (built from some-module.swift).
// This file imports that module via the .swiftinterface.

// Setup phase 1: Write the input file.
//
// RUN: echo 'public func SomeFunc() -> Int { return 42; }' >%t/some-module.swift
// RUN: echo 'public struct XWrapper {' >>%t/some-module.swift
// RUN: echo '  public let x: Int' >>%t/some-module.swift
// RUN: echo '  public init(x: Int) {' >>%t/some-module.swift
// RUN: echo '    self.x = x' >>%t/some-module.swift
// RUN: echo '  }' >>%t/some-module.swift
// RUN: echo '}' >>%t/some-module.swift

// Setup phase 2: build the module (.swiftinterface).
//
// RUN: %target-swift-frontend -I %t -emit-parseable-module-interface-path %t/SomeModule.swiftinterface -module-name SomeModule %t/some-module.swift -emit-module -o /dev/null

// Actual test: Check the CusorInfo results of references to symbols in that module
//
// RUN: %sourcekitd-test -req=cursor -pos=1:8 %s -- -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s | %FileCheck %s -check-prefix=CHECK1
// CHECK1: source.lang.swift.ref.module
// CHECK1: SomeModule
//
// RUN: %sourcekitd-test -req=cursor -pos=3:7 %s -- -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s | %FileCheck %s -check-prefix=CHECK2
// CHECK2: source.lang.swift.ref.function.free
// CHECK2: SomeFunc()
// CHECK2: () -> Int
// CHECK2: SomeModule

// RUN: %sourcekitd-test -req=cursor -pos=4:15 %s -- -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s | %FileCheck %s -check-prefix=CHECK3
// CHECK3: source.lang.swift.ref.struct
// CHECK3: XWrapper
// CHECK3: XWrapper.Type
// CHECK3: SomeModule

// RUN: %sourcekitd-test -req=cursor -pos=5:15 %s -- -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s | %FileCheck %s -check-prefix=CHECK4
// CHECK4: source.lang.swift.ref.var.instance
// CHECK4: x
// CHECK4: Int
// CHECK4: SomeModule
