// DO NOT EDIT.
// swift-format-ignore-file
//
// Generated by the Swift generator plugin for the protocol buffer compiler.
// Source: messaging/server.proto
//
// For information on using the generated types, please see the documentation:
//   https://github.com/apple/swift-protobuf/

import Foundation
import SwiftProtobuf

// If the compiler emits an error on this type, it is because this file
// was generated by a version of the `protoc` Swift plug-in that is
// incompatible with the version of SwiftProtobuf to which you are linking.
// Please ensure that you are building against the same version of the API
// that was used to generate this file.
fileprivate struct _GeneratedWithProtocGenSwiftVersion: SwiftProtobuf.ProtobufAPIVersionCheck {
  struct _2: SwiftProtobuf.ProtobufAPIVersion_2 {}
  typealias Version = _2
}

public enum Server_LogLevel: SwiftProtobuf.Enum {
  public typealias RawValue = Int
  case trace // = 0
  case debug // = 1
  case info // = 2
  case warn // = 3
  case error // = 4
  case critical // = 5
  case fatal // = 6
  case UNRECOGNIZED(Int)

  public init() {
    self = .trace
  }

  public init?(rawValue: Int) {
    switch rawValue {
    case 0: self = .trace
    case 1: self = .debug
    case 2: self = .info
    case 3: self = .warn
    case 4: self = .error
    case 5: self = .critical
    case 6: self = .fatal
    default: self = .UNRECOGNIZED(rawValue)
    }
  }

  public var rawValue: Int {
    switch self {
    case .trace: return 0
    case .debug: return 1
    case .info: return 2
    case .warn: return 3
    case .error: return 4
    case .critical: return 5
    case .fatal: return 6
    case .UNRECOGNIZED(let i): return i
    }
  }

}

#if swift(>=4.2)

extension Server_LogLevel: CaseIterable {
  // The compiler won't synthesize support with the UNRECOGNIZED case.
  public static var allCases: [Server_LogLevel] = [
    .trace,
    .debug,
    .info,
    .warn,
    .error,
    .critical,
    .fatal,
  ]
}

#endif  // swift(>=4.2)

public struct Server_DatabaseInfo {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var message: String = String()

  public var help: String = String()

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public init() {}
}

public struct Server_LogFilter {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var level: Server_LogLevel = .trace

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public init() {}
}

public struct Server_CreatureId {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var id: Data = Data()

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public init() {}
}

public struct Server_CreatureName {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var name: String = String()

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public init() {}
}

public struct Server_Creature {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var id: Data = Data()

  public var name: String = String()

  public var lastUpdated: SwiftProtobuf.Google_Protobuf_Timestamp {
    get {return _lastUpdated ?? SwiftProtobuf.Google_Protobuf_Timestamp()}
    set {_lastUpdated = newValue}
  }
  /// Returns true if `lastUpdated` has been explicitly set.
  public var hasLastUpdated: Bool {return self._lastUpdated != nil}
  /// Clears the value of `lastUpdated`. Subsequent reads from it will return its default value.
  public mutating func clearLastUpdated() {self._lastUpdated = nil}

  public var sacnIp: String = String()

  public var universe: UInt32 = 0

  public var dmxBase: UInt32 = 0

  public var numberOfMotors: UInt32 = 0

  public var motors: [Server_Creature.Motor] = []

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public enum MotorType: SwiftProtobuf.Enum {
    public typealias RawValue = Int
    case servo // = 0
    case stepper // = 1
    case UNRECOGNIZED(Int)

    public init() {
      self = .servo
    }

    public init?(rawValue: Int) {
      switch rawValue {
      case 0: self = .servo
      case 1: self = .stepper
      default: self = .UNRECOGNIZED(rawValue)
      }
    }

    public var rawValue: Int {
      switch self {
      case .servo: return 0
      case .stepper: return 1
      case .UNRECOGNIZED(let i): return i
      }
    }

  }

  public struct Motor {
    // SwiftProtobuf.Message conformance is added in an extension below. See the
    // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
    // methods supported on all messages.

    public var id: Data = Data()

    public var type: Server_Creature.MotorType = .servo

    public var number: UInt32 = 0

    public var maxValue: UInt32 = 0

    public var minValue: UInt32 = 0

    public var smoothingValue: Double = 0

    public var unknownFields = SwiftProtobuf.UnknownStorage()

    public init() {}
  }

  public init() {}

  fileprivate var _lastUpdated: SwiftProtobuf.Google_Protobuf_Timestamp? = nil
}

#if swift(>=4.2)

extension Server_Creature.MotorType: CaseIterable {
  // The compiler won't synthesize support with the UNRECOGNIZED case.
  public static var allCases: [Server_Creature.MotorType] = [
    .servo,
    .stepper,
  ]
}

#endif  // swift(>=4.2)

public struct Server_LogLine {
  // SwiftProtobuf.Message conformance is added in an extension below. See the
  // `Message` and `Message+*Additions` files in the SwiftProtobuf library for
  // methods supported on all messages.

  public var level: Server_LogLevel = .trace

  public var timestamp: SwiftProtobuf.Google_Protobuf_Timestamp {
    get {return _timestamp ?? SwiftProtobuf.Google_Protobuf_Timestamp()}
    set {_timestamp = newValue}
  }
  /// Returns true if `timestamp` has been explicitly set.
  public var hasTimestamp: Bool {return self._timestamp != nil}
  /// Clears the value of `timestamp`. Subsequent reads from it will return its default value.
  public mutating func clearTimestamp() {self._timestamp = nil}

  public var message: String = String()

  public var unknownFields = SwiftProtobuf.UnknownStorage()

  public init() {}

  fileprivate var _timestamp: SwiftProtobuf.Google_Protobuf_Timestamp? = nil
}

#if swift(>=5.5) && canImport(_Concurrency)
extension Server_LogLevel: @unchecked Sendable {}
extension Server_DatabaseInfo: @unchecked Sendable {}
extension Server_LogFilter: @unchecked Sendable {}
extension Server_CreatureId: @unchecked Sendable {}
extension Server_CreatureName: @unchecked Sendable {}
extension Server_Creature: @unchecked Sendable {}
extension Server_Creature.MotorType: @unchecked Sendable {}
extension Server_Creature.Motor: @unchecked Sendable {}
extension Server_LogLine: @unchecked Sendable {}
#endif  // swift(>=5.5) && canImport(_Concurrency)

// MARK: - Code below here is support for the SwiftProtobuf runtime.

fileprivate let _protobuf_package = "server"

extension Server_LogLevel: SwiftProtobuf._ProtoNameProviding {
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    0: .same(proto: "trace"),
    1: .same(proto: "debug"),
    2: .same(proto: "info"),
    3: .same(proto: "warn"),
    4: .same(proto: "error"),
    5: .same(proto: "critical"),
    6: .same(proto: "fatal"),
  ]
}

extension Server_DatabaseInfo: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".DatabaseInfo"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .same(proto: "message"),
    2: .same(proto: "help"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularStringField(value: &self.message) }()
      case 2: try { try decoder.decodeSingularStringField(value: &self.help) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    if !self.message.isEmpty {
      try visitor.visitSingularStringField(value: self.message, fieldNumber: 1)
    }
    if !self.help.isEmpty {
      try visitor.visitSingularStringField(value: self.help, fieldNumber: 2)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_DatabaseInfo, rhs: Server_DatabaseInfo) -> Bool {
    if lhs.message != rhs.message {return false}
    if lhs.help != rhs.help {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_LogFilter: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".LogFilter"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .same(proto: "level"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularEnumField(value: &self.level) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    if self.level != .trace {
      try visitor.visitSingularEnumField(value: self.level, fieldNumber: 1)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_LogFilter, rhs: Server_LogFilter) -> Bool {
    if lhs.level != rhs.level {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_CreatureId: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".CreatureId"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .standard(proto: "_id"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularBytesField(value: &self.id) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    if !self.id.isEmpty {
      try visitor.visitSingularBytesField(value: self.id, fieldNumber: 1)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_CreatureId, rhs: Server_CreatureId) -> Bool {
    if lhs.id != rhs.id {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_CreatureName: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".CreatureName"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .same(proto: "name"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularStringField(value: &self.name) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    if !self.name.isEmpty {
      try visitor.visitSingularStringField(value: self.name, fieldNumber: 1)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_CreatureName, rhs: Server_CreatureName) -> Bool {
    if lhs.name != rhs.name {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_Creature: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".Creature"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .standard(proto: "_id"),
    2: .same(proto: "name"),
    3: .standard(proto: "last_updated"),
    4: .standard(proto: "sacn_ip"),
    5: .same(proto: "universe"),
    6: .standard(proto: "dmx_base"),
    7: .standard(proto: "number_of_motors"),
    8: .same(proto: "motors"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularBytesField(value: &self.id) }()
      case 2: try { try decoder.decodeSingularStringField(value: &self.name) }()
      case 3: try { try decoder.decodeSingularMessageField(value: &self._lastUpdated) }()
      case 4: try { try decoder.decodeSingularStringField(value: &self.sacnIp) }()
      case 5: try { try decoder.decodeSingularUInt32Field(value: &self.universe) }()
      case 6: try { try decoder.decodeSingularUInt32Field(value: &self.dmxBase) }()
      case 7: try { try decoder.decodeSingularUInt32Field(value: &self.numberOfMotors) }()
      case 8: try { try decoder.decodeRepeatedMessageField(value: &self.motors) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    // The use of inline closures is to circumvent an issue where the compiler
    // allocates stack space for every if/case branch local when no optimizations
    // are enabled. https://github.com/apple/swift-protobuf/issues/1034 and
    // https://github.com/apple/swift-protobuf/issues/1182
    if !self.id.isEmpty {
      try visitor.visitSingularBytesField(value: self.id, fieldNumber: 1)
    }
    if !self.name.isEmpty {
      try visitor.visitSingularStringField(value: self.name, fieldNumber: 2)
    }
    try { if let v = self._lastUpdated {
      try visitor.visitSingularMessageField(value: v, fieldNumber: 3)
    } }()
    if !self.sacnIp.isEmpty {
      try visitor.visitSingularStringField(value: self.sacnIp, fieldNumber: 4)
    }
    if self.universe != 0 {
      try visitor.visitSingularUInt32Field(value: self.universe, fieldNumber: 5)
    }
    if self.dmxBase != 0 {
      try visitor.visitSingularUInt32Field(value: self.dmxBase, fieldNumber: 6)
    }
    if self.numberOfMotors != 0 {
      try visitor.visitSingularUInt32Field(value: self.numberOfMotors, fieldNumber: 7)
    }
    if !self.motors.isEmpty {
      try visitor.visitRepeatedMessageField(value: self.motors, fieldNumber: 8)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_Creature, rhs: Server_Creature) -> Bool {
    if lhs.id != rhs.id {return false}
    if lhs.name != rhs.name {return false}
    if lhs._lastUpdated != rhs._lastUpdated {return false}
    if lhs.sacnIp != rhs.sacnIp {return false}
    if lhs.universe != rhs.universe {return false}
    if lhs.dmxBase != rhs.dmxBase {return false}
    if lhs.numberOfMotors != rhs.numberOfMotors {return false}
    if lhs.motors != rhs.motors {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_Creature.MotorType: SwiftProtobuf._ProtoNameProviding {
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    0: .same(proto: "servo"),
    1: .same(proto: "stepper"),
  ]
}

extension Server_Creature.Motor: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = Server_Creature.protoMessageName + ".Motor"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .standard(proto: "_id"),
    2: .same(proto: "type"),
    3: .same(proto: "number"),
    4: .standard(proto: "max_value"),
    5: .standard(proto: "min_value"),
    6: .standard(proto: "smoothing_value"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularBytesField(value: &self.id) }()
      case 2: try { try decoder.decodeSingularEnumField(value: &self.type) }()
      case 3: try { try decoder.decodeSingularUInt32Field(value: &self.number) }()
      case 4: try { try decoder.decodeSingularUInt32Field(value: &self.maxValue) }()
      case 5: try { try decoder.decodeSingularUInt32Field(value: &self.minValue) }()
      case 6: try { try decoder.decodeSingularDoubleField(value: &self.smoothingValue) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    if !self.id.isEmpty {
      try visitor.visitSingularBytesField(value: self.id, fieldNumber: 1)
    }
    if self.type != .servo {
      try visitor.visitSingularEnumField(value: self.type, fieldNumber: 2)
    }
    if self.number != 0 {
      try visitor.visitSingularUInt32Field(value: self.number, fieldNumber: 3)
    }
    if self.maxValue != 0 {
      try visitor.visitSingularUInt32Field(value: self.maxValue, fieldNumber: 4)
    }
    if self.minValue != 0 {
      try visitor.visitSingularUInt32Field(value: self.minValue, fieldNumber: 5)
    }
    if self.smoothingValue != 0 {
      try visitor.visitSingularDoubleField(value: self.smoothingValue, fieldNumber: 6)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_Creature.Motor, rhs: Server_Creature.Motor) -> Bool {
    if lhs.id != rhs.id {return false}
    if lhs.type != rhs.type {return false}
    if lhs.number != rhs.number {return false}
    if lhs.maxValue != rhs.maxValue {return false}
    if lhs.minValue != rhs.minValue {return false}
    if lhs.smoothingValue != rhs.smoothingValue {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}

extension Server_LogLine: SwiftProtobuf.Message, SwiftProtobuf._MessageImplementationBase, SwiftProtobuf._ProtoNameProviding {
  public static let protoMessageName: String = _protobuf_package + ".LogLine"
  public static let _protobuf_nameMap: SwiftProtobuf._NameMap = [
    1: .same(proto: "level"),
    2: .same(proto: "timestamp"),
    3: .same(proto: "message"),
  ]

  public mutating func decodeMessage<D: SwiftProtobuf.Decoder>(decoder: inout D) throws {
    while let fieldNumber = try decoder.nextFieldNumber() {
      // The use of inline closures is to circumvent an issue where the compiler
      // allocates stack space for every case branch when no optimizations are
      // enabled. https://github.com/apple/swift-protobuf/issues/1034
      switch fieldNumber {
      case 1: try { try decoder.decodeSingularEnumField(value: &self.level) }()
      case 2: try { try decoder.decodeSingularMessageField(value: &self._timestamp) }()
      case 3: try { try decoder.decodeSingularStringField(value: &self.message) }()
      default: break
      }
    }
  }

  public func traverse<V: SwiftProtobuf.Visitor>(visitor: inout V) throws {
    // The use of inline closures is to circumvent an issue where the compiler
    // allocates stack space for every if/case branch local when no optimizations
    // are enabled. https://github.com/apple/swift-protobuf/issues/1034 and
    // https://github.com/apple/swift-protobuf/issues/1182
    if self.level != .trace {
      try visitor.visitSingularEnumField(value: self.level, fieldNumber: 1)
    }
    try { if let v = self._timestamp {
      try visitor.visitSingularMessageField(value: v, fieldNumber: 2)
    } }()
    if !self.message.isEmpty {
      try visitor.visitSingularStringField(value: self.message, fieldNumber: 3)
    }
    try unknownFields.traverse(visitor: &visitor)
  }

  public static func ==(lhs: Server_LogLine, rhs: Server_LogLine) -> Bool {
    if lhs.level != rhs.level {return false}
    if lhs._timestamp != rhs._timestamp {return false}
    if lhs.message != rhs.message {return false}
    if lhs.unknownFields != rhs.unknownFields {return false}
    return true
  }
}
