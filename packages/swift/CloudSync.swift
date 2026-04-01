// CloudSync.swift
// Provides the path to the CloudSync SQLite extension for use with sqlite3_load_extension.

import Foundation

public struct CloudSync {
    /// Returns the absolute path to the CloudSync dylib for use with sqlite3_load_extension.
    public static var path: String {
        #if os(macOS)
        return Bundle.main.bundlePath + "/Contents/Frameworks/CloudSync.framework/CloudSync"
        #else
        return Bundle.main.bundlePath + "/Frameworks/CloudSync.framework/CloudSync"
        #endif
    }
}
