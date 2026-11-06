// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CloudSync",
    platforms: [.macOS(.v11), .iOS(.v11), .macCatalyst(.v14)],
    products: [
        .library(
            name: "CloudSync",
            targets: ["CloudSync"])
    ],
    targets: [
        .binaryTarget(
            name: "CloudSyncBinary",
            url: "https://github.com/sqliteai/sqlite-sync/releases/download/1.2.2/cloudsync-apple-xcframework-1.2.2.zip",
            checksum: "a985836078871d10bb99f462b2a53e8b12b5c3479f06c1580758234aa22818ee"
        ),
        .target(
            name: "CloudSync",
            dependencies: ["CloudSyncBinary"],
            path: "packages/swift"
        ),
    ]
)
