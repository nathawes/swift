// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/modulecache)
// RUN: %empty-directory(%t/idx)
//
// Setup builds a parseable interface for a module SomeModule (built from some-module.swift).
// This test report references to symbols from the .swiftmodule loaded via a .swiftinterface

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
// RUN: %target-swift-frontend -I %t -index-store-path %t/idx -emit-parseable-module-interface-path %t/SomeModule.swiftinterface -module-name SomeModule %t/some-module.swift -emit-module -o /dev/null

// Actual test: check we generate the correct index data for SomeModule
//
// %swift-ide-test_plain -print-indexed-symbols -source-filename %s -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface | %FileCheck %s
// RUN: %target-swift-frontend -typecheck -index-store-path %t/idx -I %t -module-cache-path %t/modulecache -enable-parseable-module-interface %s
// RUN: c-index-test core -print-unit %t/idx | %FileCheck -check-prefix=UNIT %s
// RUN: c-index-test core -print-record %t/idx | %FileCheck -check-prefix=RECORD %s


// UNIT: module-name: index_swiftinterface
// UNIT: has-main: 1
// UNIT: main-path: SOURCE_DIR/test/Index/index_swiftinterface.swift
// UNIT: DEPEND START
// UNIT: Unit | system | Swift | {{.*}}Swift.swiftmodule
// UNIT: Unit | user | SomeModule | {{.*}}/modulecache/SomeModule-{{.*}}.swiftmodule
// UNIT: Record | user | SOURCE_DIR/test/Index/index_swiftinterface.swift | index_swiftinterface.swift-
// UNIT: DEPEND END (3)


// UNIT: module-name: SomeModule
// UNIT: has-main: 1
// UNIT: main-path: {{.*}}/some-module.swift
// UNIT: DEPEND START
// UNIT: Unit | system | Swift | {{.*}}Swift.swiftmodule
// UNIT: Record | user | {{.*}}/some-module.swift | some-module.swift-
// UNIT: DEPEND END (2)

// RECORD: index_swiftinterface.swift
// RECORD-NEXT: ------------
// RECORD: module/Swift | SomeModule | [[SomeModule_USR:.*]] | <no-cgname> | Ref - 
// RECORD: function/Swift | SomeFunc() | [[SomeFunc_USR:.*]] | <no-cgname> | Ref,Call - 
// RECORD: variable/Swift | wrapper | [[wrapper_USR:.*]] | <no-cgname> | Def,Ref,Read - RelChild,RelAcc
// RECORD: struct/Swift | XWrapper | [[XWrapper_USR:.*]] | <no-cgname> | Ref - RelRec
// RECORD: constructor/Swift | init(x:) | [[XWrapper_init_USR:.*]] | <no-cgname> | Ref,Call - 
// RECORD: instance-property/Swift | x | [[XWrapper_x_USR:.*]] | <no-cgname> | Ref,Read - 

// RECORD: [[@LINE+1]]:8 | module/Swift | [[SomeModule_USR]] | Ref | rel: 0
import SomeModule

// RECORD: [[@LINE+1]]:7 | function/Swift | [[SomeFunc_USR]] | Ref,Call | rel: 0
print(SomeFunc())

// RECORD: [[@LINE+3]]:5 | variable/Swift | [[wrapper_USR]] | Def | rel: 0
// RECORD: [[@LINE+2]]:15 | struct/Swift | [[XWrapper_USR]] | Ref | rel: 0
// RECORD: [[@LINE+1]]:15 | constructor/Swift | [[XWrapper_init_USR]] | Ref,Call | rel: 0
let wrapper = XWrapper(x: 43)

// RECORD: [[@LINE+2]]:7 | variable/Swift | [[wrapper_USR]] | Ref,Read | rel: 0
// RECORD: [[@LINE+1]]:15 | instance-property/Swift | [[XWrapper_x_USR]] | Ref,Read | rel: 0
print(wrapper.x)

// RECORD: some-module.swift
// RECORD: ------------
// RECORD: function/Swift | SomeFunc() | [[SomeFunc_USR]] | <no-cgname> | Def - RelCont
// RECORD: struct/Swift | XWrapper | [[XWrapper_USR]] | <no-cgname> | Def - RelChild,RelRec
// RECORD: instance-property/Swift | x | [[XWrapper_x_USR]] | <no-cgname> | Def,Ref,Writ,RelChild,RelCont - RelChild,RelAcc
// RECORD: constructor/Swift | init(x:) | [[XWrapper_init_USR]] | <no-cgname> | Def,RelChild - RelChild,RelCall,RelCont

