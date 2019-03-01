// RUN: %empty-directory(%t)
//
// Setup builds a parseable interface for a module SomeModule (built from some-module.swift).
// This test report references to symbols from the .swiftmodule loaded via a .swiftinterface

// Setup phase 1: Write the input file.
//
// RUN: echo 'public func someFunc() -> Int { return 42; }' >%t/some-module.swift
// RUN: echo 'public struct XWrapper {' >>%t/some-module.swift
// RUN: echo '  public let x: Int' >>%t/some-module.swift
// RUN: echo '  public init(x: Int) {' >>%t/some-module.swift
// RUN: echo '    self.x = x' >>%t/some-module.swift
// RUN: echo '  }' >>%t/some-module.swift
// RUN: echo '}' >>%t/some-module.swift

// Setup phase 2: build the module (.swiftinterface).
//
// RUN: %target-swift-frontend -emit-parseable-module-interface-path %t/SomeModule.swiftinterface -module-name SomeModule %t/some-module.swift -emit-module -o /dev/null

// Actual test: check we generate the correct index data for SomeModule
//
// %target-swift-ide-test -print-indexed-symbols -module-to-print SomeModule -source-filename -source-filename %s -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface | %FileCheck %s
// RUN: %target-swift-frontend -typecheck -index-system-modules -index-store-path %t/idx -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s
// RUN: c-index-test core -print-unit %t/idx | %FileCheck -check-prefix=UNIT %s
// RUN: c-index-test core -print-record %t/idx | %FileCheck -check-prefix=RECORD %s

import SomeModule

print(someFunc())

// UNIT: djsjhdf
// RECORD: lsjsdjfh
