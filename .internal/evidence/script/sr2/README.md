# SR-2 原始证据

本目录补交 SR-2 原始验证包，文件内容未重新生成、改标签或替换。修正后的 SR-2 和 SR-3
验证将使用各自的新归档；本包的 production candidate 仍为
`feaa7550bd819b0bcb928d477fb45e40ff9282b1`，报告提交为
`1a7fbd60f47a115b7b01528075550b77c1a3c194`。

- [原始日志 ZIP](SR2-raw-evidence.zip)：1,512,316 bytes，738 项归档条目，无编译二进制。
- [737 项文件 SHA-256 索引](raw-files.json)：逐项对应 ZIP 内相对路径。
- [源码、依赖与安装身份](identity.json)。
- [ZIP SHA-256](SHA256SUMS.txt)。

ZIP SHA-256：`b80eef0639b910db42545dd71794f2be8f6f9ddd3dad0e7f87cccf0bb0a31deb`。

索引覆盖原始 qualification、14 个 installed consumers、wire golden、lifecycle/Event trace、
性能与分配诊断，以及失败尝试。压缩包内的绝对 Windows 路径记录当次执行身份；审阅者
读取归档无需访问这些本地目录。六项 Scene Lua 失败保留在包内，不能将结果解读为全绿。

固定提交下载入口及远端取回校验记录将在归档提交推送后补在本索引。
