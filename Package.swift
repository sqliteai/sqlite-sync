// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CloudSync",
    platforms: [.macOS(.v11), .iOS(.v11)],
    products: [
        .library(
            name: "CloudSync",
            targets: ["CloudSync"])
    ],
    targets: [
        .binaryTarget(
            name: "CloudSyncBinary",
            url: "https://github.com/sqliteai/sqlite-sync/releases/download/1.0.5/cloudsync-apple-xcframework-1.0.5.zip",
            checksum: "8efd5da9e69731f0ed32d0e137314e260c2c8d763c5dc07672092d642418364b"
        ),
        .target(
            name: "CloudSync",
            dependencies: ["CloudSyncBinary"],
            path: "packages/swift"
        ),
    ]
)
