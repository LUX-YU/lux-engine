#pragma once

#include "Asset.hpp"
#include "AssetRef.hpp"
#include <lux/engine/resource/asset/codecs/AssetCodecCatalog.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>
#include <lux/engine/resource/asset/visibility.h>

namespace lux::asset
{
    /**
     * @brief Forward declaration of the AssetManagerImpl class.
     */
    class AssetManagerImpl;

    class AssetVfs;

    // (曾有 SubscriptionToken + onLoaded/onWillUnload 回调类型 —— 批E2 退役:
    //  AssetHandle 时代遗物,生产使用面归零。消费者每帧按 id 重查(resolver
    //  模式)+ AssetRef 驻留票;「对象要没了」的事实走 setBroadcast 的
    //  invalidated 广播 → 总线事件。)

    /**
     * @brief Manages assets and resources throughout the application lifecycle.
     *
     * The AssetManager provides a centralized interface for loading, storing, and retrieving
     * assets. It handles asset lifetime management, caching, and provides type-safe access
     * to different asset types such as textures, materials, meshes, and models.
     */
    class LUX_ASSET_PUBLIC AssetManager
    {
        // Grant access to AssetSerDeser for serialization/deserialization.
        friend class AssetSerDeser;

    public:
        /**
         * @brief Constructs a new AssetManager instance with default settings.
         */
        explicit AssetManager(std::shared_ptr<const AssetCodecCatalog> codecs);

        /**
         * @brief Destructs the AssetManager instance and releases all managed resources.
         */
        ~AssetManager();

        // Copy operations are deleted to enforce unique asset management.
        AssetManager(const AssetManager&) = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        /// Create through this manager's immutable product codec snapshot.
        [[nodiscard]] std::unique_ptr<AssetSerDeser> createSerDeser(
            EAssetType type,
            std::shared_ptr<AssetManager> owner
        ) const;

        [[nodiscard]] const AssetCodecCatalog& codecCatalog() const noexcept;

        [[nodiscard]] std::shared_ptr<const AssetCodecCatalog>
        codecCatalogOwner() const noexcept;

        // 这里曾有一个 `template<typename T> T* queryData(const asset_id_t&)`,
        // 文档说它是"调用非模板 queryData 的便利包装"。它**根本不可编译**:
        // 函数体里那个不带显式模板实参的 `queryData(id)` 只能解析到私有的
        // `void* queryData(const asset_id_t&)`(见下方 private 段),而
        // `void*` → `T*` 是 ill-formed。因为全仓零调用点、从没被实例化,
        // 编译器至今没有机会拒绝它 —— 它只是在头文件里向读者承诺一个不存在的能力。
        // (私有的那个 void* 版本是真实现、有调用方,保留。)

        /**
         * @brief Checks if asset data exists for the given asset ID.
         *
         * @param id The unique asset identifier.
         * @return true if the asset data exists, false otherwise.
         */
        [[nodiscard]] bool hasData(const asset_id_t& id) const;

        /**
         * @brief Checks if an asset with the given asset ID exists.
         *
         * @param id The unique asset identifier.
         * @return true if the asset exists, false otherwise.
         */
        [[nodiscard]] bool hasAsset(const asset_id_t& id) const;

        /**
         * @brief Retrieves constant asset information for a given asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant AssetInfo.
         */
        [[nodiscard]] const AssetInfo* queryInfo(const asset_id_t& id) const;

        /**
         * @brief Fetches a mutable asset using its asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the asset.
         */
        [[nodiscard]] LuxAsset* fetchAsset(const asset_id_t& id);
        /**
         * @brief Fetches a constant asset using its asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset.
         */
        [[nodiscard]] const LuxAsset* fetchAsset(const asset_id_t& id) const;

        /**
         * @brief Fetches an asset and casts it to the specified type.
         *
         * Returns nullptr if the asset does not exist or the type does not match.
         *
         * @tparam T The type derived from LuxAsset.
         * @param id The unique asset identifier.
         * @return Pointer to the asset of type T, or nullptr on failure.
         */
        template<typename T>
        T* fetchAssetAs(const asset_id_t& id)
            requires std::is_base_of_v<LuxAsset, T>
        {
            LuxAsset* asset = fetchAsset(id);
            if (asset == nullptr)
                return nullptr;

            if (asset->type() != T::asset_type)
                return nullptr;

            return static_cast<T*>(asset);
        }

        /**
         * @brief Fetches a constant asset and casts it to the specified type.
         *
         * Returns nullptr if the asset does not exist or the type does not match.
         *
         * @tparam T The type derived from LuxAsset.
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset of type T, or nullptr on failure.
         */
        template<typename T>
        const T* fetchAssetAs(const asset_id_t& id) const
            requires std::is_base_of_v<LuxAsset, T>
        {
            const LuxAsset* asset = fetchAsset(id);
            if (asset == nullptr)
                return nullptr;

            if (asset->type() != T::asset_type)
                return nullptr;

            return static_cast<const T*>(asset);
        }

        // 同上,`template<typename T> T* queryInfoAs(const asset_id_t&) const` 也删了,
        // 同一个病的第二例:`static_cast<T*>(queryInfo(id))` —— queryInfo 返回
        // `const AssetInfo*`,static_cast 到非 const 的 `T*` 是**丢 const**,
        // 同样 ill-formed、同样零调用点、同样从没被实例化过。
        // 需要按类型取 info 的调用方用 fetchAssetAs<T>()(它有真正的 type() 检查)。

        /**
         * @brief Generates a unique asset identifier (UUID).
         *
         * @return The generated asset_id_t.
         */
        [[nodiscard]] asset_id_t generateUUID() const;

        /**
         * @brief Generates a deterministic (name-based) UUID from @p seed.
         *
         * The same seed always maps to the same UUID under a fixed engine
         * namespace. The import pipeline feeds a content-relative identity
         * (e.g. "Models/CesiumMan|mesh|0") so re-importing a source reproduces
         * identical ids and the on-disk reference graph survives. Runtime asset
         * creation should keep using the random generateUUID() overload.
         *
         * @param seed Stable identity string for the asset.
         * @return The deterministic asset_id_t.
         */
        [[nodiscard]] asset_id_t generateUUID(std::string_view seed) const;

        /**
         * @brief Creates a new AssetInfo instance for the given asset type.
         *
         * The function automatically sets the current asset version, generates a new UUID, assigns
         * the asset type, and records the current timestamp.
         *
         * @param type The asset type.
         * @return A unique_ptr to the newly created AssetInfo.
         */
        std::unique_ptr<AssetInfo> createAssetInfo(EAssetType type) const
        {
            return createAssetInfo(type, std::string_view{});
        }

        /**
         * @brief Creates a new AssetInfo, optionally with a deterministic id.
         *
         * When @p seed is non-empty the id is derived deterministically from it
         * (see generateUUID(std::string_view)); when empty, a random id is used
         * — so existing callers keep their behavior unchanged.
         *
         * @param type The asset type.
         * @param seed Stable identity string, or empty for a random id.
         * @return A unique_ptr to the newly created AssetInfo.
         */
        std::unique_ptr<AssetInfo> createAssetInfo(EAssetType type, std::string_view seed) const
        {
            auto current_time = std::chrono::system_clock::now().time_since_epoch();
            // Convert the current time to nanoseconds.
            auto date = std::chrono::duration_cast<std::chrono::nanoseconds>(current_time).count();

            auto info = std::make_unique<AssetInfo>();

            info->id = seed.empty() ? generateUUID() : generateUUID(seed);
            info->type = type;
            info->date = date;

			return info;
        }

        template<typename T, typename... Args>
        std::unique_ptr<LuxAsset> createAsset(std::unique_ptr<typename T::asset_data_t> data, Args&&... args)
        {
			auto info  = createAssetInfo(T::asset_type);
			auto asset = std::make_unique<T>(
                std::move(info),
                std::forward<Args>(args)...
            );
			asset->setData(std::move(data));
			return asset;
        }

        /**
         * @brief Like createAsset, but derives a deterministic id from @p seed.
         *
         * An empty seed falls back to a random id (identical to createAsset), so
         * this is a safe drop-in for import paths that may or may not request
         * determinism.
         */
        template<typename T, typename... Args>
        std::unique_ptr<LuxAsset> createAssetSeeded(std::string_view seed,
            std::unique_ptr<typename T::asset_data_t> data, Args&&... args)
        {
			auto info  = createAssetInfo(T::asset_type, seed);
			auto asset = std::make_unique<T>(
                std::move(info),
                std::forward<Args>(args)...
            );
			asset->setData(std::move(data));
			return asset;
        }

        /**
         * @brief Registers a new asset with the AssetManager.
         *
         * Transfers ownership of the asset via a std::unique_ptr.
         *
         * @param asset The asset to register.
         * @return true if the asset was successfully registered, false otherwise.
         */
        bool registerAsset(std::unique_ptr<LuxAsset> asset);

        /**
         * @brief Remove (evict) an asset from the manager, freeing its CPU memory.
         *
         * After this call returns the asset's CPU data is gone and any raw
         * pointer obtained via fetchAsset / queryData is invalid. If no asset
         * exists for the id, this is a no-op (no broadcast). 账本(引用计数)
         * **不**被本方法清 —— 兴趣比对象活得久(裁决七);同时经广播回调发
         * 一条**失效**事实(on_invalidated),消费者在总线订阅侧释放缓存的
         * 指针/句柄。
         *
         * @param id The asset to remove.
         */
        void removeAsset(const asset_id_t& id);

        /**
         * @brief Drop an asset's CPU data back to a SHELL (keep it registered).
         *
         * Open-world (W2c) eviction: unlike removeAsset (which deletes the asset and
         * dangles its UUID), this keeps the info-only shell registered and only
         * frees the decoded CPU data — so the next requestLoad(id) re-decodes it.
         * No callback fires (the shell stays present; consumers re-query by id and
         * tolerate a null data() — the renderable bridge / anim resolver already
         * do). Guarded inside the asset: a no-op (returns false) if the asset is
         * absent or already data-less. Main-thread only (same contract as
         * registerAsset).
         *
         * @return true if data was dropped; false if no-op / unsafe.
         */
        [[nodiscard]] bool unloadData(const asset_id_t& id);

        // ─── 引用计数：谁还需要这个资产活着 ──────────────────────────────
        //
        // 一本账，问题只有一个：**还有东西在用它吗**。答案有两个消费者 ——
        // 流送要它来决定能不能驱逐 CPU 数据（`unloadData`），GPU 上传缓存要它来
        // 决定什么时候销毁上传出去的资源。
        //
        // 此前这本账在**渲染**那边（`GpuResourceCache` 四套缓存各带一个 refcount
        // 字段 + 三对 acquire/release + 材质→贴图级联）。于是出现一个倒挂：世界
        // 流送要决定「这份 CPU 数据能不能驱逐」，却得去问渲染系统。资产的生命
        // 周期归拥有资产的那一层，这里才是它该在的地方。
        //
        // 加减的**唯一入口是 `acquire` 发出的 AssetRef 票据**（持有 = +1，析构/
        // 重置 = −1）。此前是公开的 retain/release 动词对，每个调用方手工配平
        // 「先钉新再放旧」「离场必还」——漂账只在泄漏统计里可见；RAII 让忘还票
        // 在语法上不可表达。谁持票：**组件的变化**——资源解析器在实体开始引用
        // 某资产时取票、不再引用时票随替换/离场自然还掉；GPU 上传缓存自己也是
        // 持票者（上传材质时取贴图的票，销毁条目时票随条目析构），级联因此自然
        // 成立，不需要第二本账。
        //
        // **账本与注册表解耦（裁决七的前提）**：`removeAsset` 不清计数——兴趣比
        // 对象活得久。资产对象死后，持票者随后松手时计数照常流干、归零广播照常
        // 触发（派生 GPU 副本靠这个边沿回收）。清了账，还在持有的票释放时就等
        // 不到归零边沿，GPU 副本必漏。
        //
        // 线程：与 registerAsset 同一条约定 —— **主线程**（debug 下入口断言）。

        /// 取一张驻留票（+1）。nil id 返回空票（合法，no-op）。
        /// 票的拷贝/析构语义见 AssetRef.hpp。
        [[nodiscard]] AssetRef acquire(const asset_id_t& id);

        /// 还有人引用吗。**驱逐 CPU 数据前必须问这一句** —— 一个共享的网格/材质
        /// 可能在某个休眠单元的实体被回收之后，仍然活在另一个激活单元里。
        [[nodiscard]] bool isReferenced(const asset_id_t& id) const noexcept;

        // ─── 广播回调缝(统一事件系统批E:三队列退役)────────────────────
        //
        // 本模块零事件概念(裁决③):账本事实经这三个回调交给**宿主组装层**,
        // 由它翻译成总线事件(AssetEvents.hpp);消费者(GPU 上传缓存等)在
        // 帧泵上订阅。旧三队列「为什么是队列而不是回调」的理由 —— 「销毁命令
        // 必须在构建器开着时发,而 release 可能发生在观察者里(构建器没开)」
        // —— 由总线原样承接:publish 任意点安全(无锁入队),handler 恒在帧
        // OPEN 的泵排空点执行。多消费者(单队列时代抢不到的静默泄漏)由总线
        // 的多订阅承接。
        //
        // 回调在**账本线程**(主线程)同步调,时机 = 事实发生的那一刻;
        // 三个语义互不混用(见 AssetEvents.hpp)。装配期注入一次;removeAsset
        // 不清账、「兴趣比对象活得久」(裁决七)等账本契约不变。

        struct BroadcastCallbacks
        {
            /// 计数 1→0(派生物回收边沿)。
            std::function<void(const asset_id_t&)> on_unreferenced;
            /// removeAsset 扩散(对象没了;账本不清)。
            std::function<void(const asset_id_t&)> on_invalidated;
            /// 内容变更(对象在场;@p revision 为 bump 后的新值)。
            std::function<void(const asset_id_t&, std::uint32_t revision)>
                on_content_changed;
            /// 注册成功(registerAsset;驻留 T2)。给「删了又回来」的解封
            /// 消费者一个**推式**信号 —— 此前失效封印的解除靠消费端每帧
            /// queryInfo 轮询检测资产回归。replaceAsset **不**触发(对象
            /// 一直在场,那是 content_changed 的语义);注册失败(重复 id/
            /// 空 info)不触发。
            std::function<void(const asset_id_t&)> on_registered;
        };

        /// 装配期注入一次(主线程)。空回调 = 对应事实静默丢弃 —— 宿主必须
        /// 三个都接(忘记装配的失败模式是 GPU 副本泄漏/热更新失灵,探针钉)。
        void setBroadcast(BroadcastCallbacks callbacks);

        // ─── 内容变更(热更新批4)─────────────────────────────────────────
        //
        // 信号源是作者语义的出口(文件监视看到 .luxasset 变化 → replaceAsset;
        // 或编辑器内存路径显式 notifyContentChanged)。

        /// @p id 当前的内容 revision(运行期计数,进程起始为 0;每次内容变更
        /// +1,removeAsset 也 +1 以钉「删了又回来」的失配)。派生缓存在发起
        /// 上传时盖章,失配即重建。
        [[nodiscard]] std::uint32_t contentRevision(const asset_id_t& id) const;

        /// 显式宣告 @p id 的内容变了(++revision + 广播)。给「就地 setData」
        /// 的内存路径用;文件路径优先走 replaceAsset。
        void notifyContentChanged(const asset_id_t& id);

        /// 同 id 换对象:旧对象析构、新对象顶入、++revision + 广播。文件监视
        /// /重导入的落点。**不动引用计数、不推失效队列**(对象一直在场,票照
        /// 常有效)。id 未注册返回 false(那是 registerAsset 的活)。
        bool replaceAsset(std::unique_ptr<LuxAsset> asset);

        // (曾有 onLoaded/onWillUnload/unsubscribe 生命周期订阅面 —— 批E2
        //  退役,见文件头部的同名注释。)

        // ─── Virtual filesystem (id -> locator, replacing id -> file) ──────
        //
        // The manager stays a pure in-memory id->asset map; the VFS is the
        // pluggable answer to "where do absent assets come from". The editor
        // mounts an Authoring loose provider over the content folder; shipped builds
        // mount PakAssetProvider — same manager code either way.

        /**
         * @brief Attach (or detach with nullptr) the asset VFS used by
         *        findAssetByPath / ensureAsset.
         */
        void setVfs(std::shared_ptr<const AssetVfs> vfs);

        /**
         * @brief The attached VFS, or nullptr.
         */
        [[nodiscard]] std::shared_ptr<const AssetVfs> vfs() const;

        /**
         * @brief Resolve an absolute virtual path ("/Game/Materials/M_Box")
         *        to an asset id. PURE resolution — never loads, never touches
         *        disk beyond the provider's in-memory tables. Nil id when no
         *        VFS is attached, the path is illegal, or nothing matches.
         */
        [[nodiscard]] asset_id_t findAssetByPath(std::string_view vpath) const;

        /**
         * @brief Get-or-load: returns the present asset, or loads it through
         *        the VFS (open -> magic dispatch -> SerDeser -> registerAsset)
         *        and returns it.
         *
         * Main-thread contract (same as registerAsset). Errors:
         * ASSET_NOT_EXIST when absent and no VFS / no provider has the id;
         * otherwise the provider's open error or the SerDeser's parse error.
         */
        [[nodiscard]] lux::cxx::expected<LuxAsset*, EAssetError>
        ensureAsset(const asset_id_t& id);

    private:
        // 账本的裸动词。只有 AssetRef 的票据生命周期能碰 —— 公开面只剩
        // `acquire`。此前它们是公开 API，每个调用方各自配平（见上面契约段）。
        friend class AssetRef;
        void retain(const asset_id_t& id);
        std::uint32_t release(const asset_id_t& id);

        /**
         * @brief Internal function to query asset data.
         *
         * Returns a void pointer to the asset data associated with the given asset ID.
         *
         * @param id The unique asset identifier.
         * @return Void pointer to the asset data.
         */
        void* queryData(const asset_id_t& id);

        /**
         * @brief Internal function to query an asset by its identifier.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the asset.
         */
        LuxAsset* queryAsset(const asset_id_t& id);
        /**
         * @brief Const version of queryAsset.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset.
         */
        const LuxAsset* queryAsset(const asset_id_t& id) const;

        /**
         * @brief Pointer to the internal implementation of AssetManager.
         *
         * Uses the Pimpl idiom to hide implementation details.
         */
        std::unique_ptr<AssetManagerImpl> impl_;
    };
}
