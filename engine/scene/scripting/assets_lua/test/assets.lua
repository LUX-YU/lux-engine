---@lux.requires lux.scene.assets
---@lux.requires lux.scene.skeletons
---@lux.requires lux.simulation.delay
EC2Assets = {}

---@lux.method
---@lux.coroutine
---@return void
function EC2Assets:run()
    local id = lux.Assets.assetId(0xfedcba98, 0x76544abc, 0xffffffff, 0xffffffff)
    assert(lux.Assets.assetWord(id, 0) == 0xfedcba98)
    assert(lux.Assets.assetWord(id, 3) == 0xffffffff)
    local result = lux.Skeletons.read(id)
    assert(lux.Assets.succeeded(result))
    local handle = lux.Assets.handle(result)
    assert(lux.Skeletons.boneCount(handle) == 2)
    assert(lux.Skeletons.parentIndex(handle, 0) == -1)
    assert(lux.Skeletons.parentIndex(handle, 1) == 0)
    assert(lux.Skeletons.parentIndex(handle, 2) == -9)
    assert(lux.Assets.inspectionError(lux.Assets.describeAsset(handle)) == 0)
    assert(lux.Assets.assetWord(lux.Assets.inspectedId(lux.Assets.describeAsset(handle)), 3) == 0xffffffff)
    local before = self:get_component("Counter")
    lux.Delay.nextStep()
    assert(lux.Skeletons.boneCount(handle) == 2)
    assert(self:patch_component("Counter", before + 2))
    assert(self:get_component("Counter") == before) -- original deferred command barrier
    lux.Assets.releaseAsset(handle)
    lux.Assets.releaseAsset(handle) -- idempotent script release
    assert(lux.Assets.inspectionError(lux.Assets.describeAsset(handle)) == 4)
    local missing = lux.Assets.readAsset(lux.Assets.assetId(1, 2, 3, 4))
    assert(not lux.Assets.succeeded(missing))
    assert(lux.Assets.errorDomain(missing) == 1)
    assert(lux.Assets.errorCode(missing) == 0) -- original EAssetStorageError.NOT_FOUND, within STORAGE domain
    local raw = lux.Assets.readAsset(id)
    assert(lux.Assets.succeeded(raw))
    local raw_handle = lux.Assets.handle(raw)
    local bytes = lux.Assets.copyAssetBytes(raw_handle, 0, 0, 4)
    assert(lux.Assets.bytesError(bytes) == 0 and lux.Assets.bytesCount(bytes) == 4)
    assert(lux.Assets.byteAt(bytes, 0) >= 0 and lux.Assets.byteAt(bytes, 4) == -1)
    assert(lux.Assets.bytesError(lux.Assets.copyAssetBytes(raw_handle, 0xffffffff, 0xffffffff, 1)) == 7)
    lux.Assets.releaseAsset(raw_handle)
end
