from pathlib import Path
import re, subprocess
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def read(p): return (s/p).read_text(encoding='utf-8-sig')
def write(p,t): (s/p).write_text(t,encoding='utf-8',newline='\n')
paths=subprocess.check_output(['git','ls-files'],cwd=s,text=True).splitlines()
for p in paths:
    if not p.startswith(('editor/','cmake/','docs/')) or not p.endswith(('.hpp','.cpp','.txt','.cmake','.json','.py','.md')): continue
    text=read(p)
    new=re.sub(r'\bProjectPublication\b','PreparedProjectPublication',text) if not p.endswith('ProjectPublication.cpp') else text
    # The file is still the publication theme; only the replaced public type changes.
    new=new.replace('storage/PreparedProjectPublication.hpp','storage/ProjectPublication.hpp')
    new=new.replace('src/PreparedProjectPublication.cpp','src/ProjectPublication.cpp')
    new=new.replace('ProjectOpenData','PreparedProjectOpen').replace('readPreparedProjectOpen','prepareProjectOpen')
    if new!=text:write(p,new)
for folder,ext in [('include/lux/engine/editor/storage','hpp'),('src','cpp')]:
    root=s/'editor/activities/project'/folder
    (root/f'ProjectOpenData.{ext}').rename(root/f'PreparedProjectOpen.{ext}')
p='editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp'
t=read(p);start=t.index('    struct LUX_EDITOR_STORAGE_PUBLIC PreparedProjectPublication final');end=t.index('    struct ProjectPublicationReceipt final',start)
t=t[:start]+'''    // Validated, encoded once, and shared read-only. It carries no live Project authority.
    class LUX_EDITOR_STORAGE_PUBLIC ProjectPublicationPlan final
    {
    public:
        using PrepareResult = EditorResult<std::shared_ptr<const ProjectPublicationPlan>>;
        [[nodiscard]] static PrepareResult prepare(
            std::filesystem::path root,
            std::string manifest_path,
            std::string before_manifest_digest,
            ProjectManifest manifest,
            std::vector<ProjectFileChange> files,
            std::vector<std::string> package_paths = {}
        );
        [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
        [[nodiscard]] const std::string& manifestPath() const noexcept { return manifest_path_; }
        [[nodiscard]] const std::string& beforeManifestDigest() const noexcept { return before_manifest_digest_; }
        [[nodiscard]] const ProjectManifest& manifest() const noexcept { return manifest_; }
        [[nodiscard]] const cxx::SharedBytes<>& manifestBytes() const noexcept { return manifest_bytes_; }
        [[nodiscard]] std::span<const ProjectFileChange> files() const noexcept { return files_; }
        [[nodiscard]] std::span<const std::string> packagePaths() const noexcept { return package_paths_; }

    private:
        ProjectPublicationPlan() = default;
        std::filesystem::path root_;
        std::string manifest_path_;
        std::string before_manifest_digest_;
        ProjectManifest manifest_;
        cxx::SharedBytes<> manifest_bytes_;
        std::vector<ProjectFileChange> files_;
        std::vector<std::string> package_paths_;
    };

    // The reservation stays on the Project owner lane. Workers may own only sharePlan().
    class LUX_EDITOR_STORAGE_PUBLIC PreparedProjectPublication final
    {
    public:
        PreparedProjectPublication() noexcept = default;
        ~PreparedProjectPublication();
        PreparedProjectPublication(PreparedProjectPublication&&) noexcept;
        PreparedProjectPublication& operator=(PreparedProjectPublication&&) noexcept;
        PreparedProjectPublication(const PreparedProjectPublication&) = delete;
        PreparedProjectPublication& operator=(const PreparedProjectPublication&) = delete;
        [[nodiscard]] const ProjectPublicationPlan& plan() const noexcept { return *plan_; }
        [[nodiscard]] std::shared_ptr<const ProjectPublicationPlan> sharePlan() const noexcept { return plan_; }

    private:
        friend class ProjectStorage;
        std::shared_ptr<const ProjectPublicationPlan> plan_;
        ProjectStorage* owner_{};
    };

'''+t[end:]
t=t.replace('        std::vector<ProjectPackage> packages;\n    };','        std::vector<ProjectPackage> packages;\n        std::shared_ptr<const ProjectPublicationPlan> plan;\n    };')
t=t.replace('        const PreparedProjectPublication&,','        std::shared_ptr<const ProjectPublicationPlan>,')
write(p,t)
p='editor/activities/project/src/ProjectStorage.cpp';t=read(p)
start=t.index('    PreparedProjectPublication::PreparedProjectPublication(');end=t.index('    std::string_view ProjectStorage::sourceDigest',start)
t=t[:start]+'''    PreparedProjectPublication::PreparedProjectPublication(PreparedProjectPublication&& other) noexcept
        : plan_(std::move(other.plan_)), owner_(std::exchange(other.owner_, nullptr))
    {}

    PreparedProjectPublication& PreparedProjectPublication::operator=(PreparedProjectPublication&& other) noexcept
    {
        if (this != &other)
        {
            PreparedProjectPublication released(std::move(*this));
            plan_ = std::move(other.plan_);
            owner_ = std::exchange(other.owner_, nullptr);
        }
        return *this;
    }

'''+t[end:]
t=t.replace('        PreparedProjectPublication publication;\n        publication.root = root_;\n        publication.manifest_path = source_.file.filename().generic_string();\n        publication.before_manifest_digest = source_.manifest_digest;\n        publication.manifest = std::move(next);','        std::vector<std::string> package_paths;')
t=t.replace('publication.manifest.assets','next.assets').replace('publication.package_paths.push_back','package_paths.push_back')
t=t.replace('        publication.files = std::move(update.files);','''        auto plan = ProjectPublicationPlan::prepare(
            root_, source_.file.filename().generic_string(), source_.manifest_digest,
            std::move(next), std::move(update.files), std::move(package_paths)
        );
        if (!plan)
            return cxx::unexpected(std::move(plan.error()));
        PreparedProjectPublication publication;
        publication.plan_ = std::move(*plan);''')
t=t.replace('        if (receipt.manifest != publication.manifest || receipt.packages.size() != publication.package_paths.size())','''        const auto& plan = publication.plan();
        const bool is_wrong_plan = receipt.plan.get() != publication.plan_.get();
        const bool is_stale_baseline = source_.manifest_digest != plan.beforeManifestDigest();
        const bool is_wrong_manifest = receipt.manifest != plan.manifest() ||
            receipt.manifest_digest != projectContentDigest(plan.manifestBytes().view());
        const bool is_wrong_packages = receipt.packages.size() != plan.packagePaths().size();
        const bool is_invalid_receipt = is_wrong_plan || is_stale_baseline || is_wrong_manifest || is_wrong_packages;
        if (is_invalid_receipt)''')
t=t.replace('publication.package_paths[index]','plan.packagePaths()[index]')
write(p,t)
p='editor/activities/project/src/ProjectPublicationOperation.cpp';t=read(p)
for field,method in {'manifest':'manifest','files':'files','root':'root','package_paths':'packagePaths','manifest_path':'manifestPath','before_manifest_digest':'beforeManifestDigest','manifest_bytes':'manifestBytes'}.items():
    t=re.sub(r'publication_\.'+field+r'\b',f'publication_.plan().{method}()',t)
t=t.replace('            cxx::SharedBytes<> manifest;\n','')
t=t.replace('            receipt_.manifest = publication_.plan().manifest();','            receipt_.manifest = publication_.plan().manifest();\n            receipt_.plan = publication_.sharePlan();')
t=t.replace('                         root = publication_.plan().root(),\n                         paths = publication_.plan().packagePaths(),\n                         manifest = publication_.plan().manifest()','                         plan = publication_.sharePlan()')
t=t.replace('[root = std::move(root), paths = std::move(paths), manifest = std::move(manifest)](', '[plan = std::move(plan)](')
t=t.replace('for (const auto& path : paths)','for (const auto& path : plan->packagePaths())').replace('readProjectPackage(root, path)','readProjectPackage(plan->root(), path)')
a=t.index('                                    auto encoded = encodeProjectManifest(manifest);');b=t.index('                                    return result;',a)
t=t[:a]+t[b:]
t=t.replace('                publication_.plan().manifestBytes() = std::move((**prepared_).manifest);\n','')
write(p,t)
p='editor/activities/project/src/ProjectPublication.cpp';t=read(p)
t=t.replace('        const ProjectPublication& publication,','        std::shared_ptr<const ProjectPublicationPlan> plan,')
start=t.index('        auto manifest_bytes = publication.manifest_bytes;');end=t.index('        if (files.size() > 4096)',start)
t=t[:start]+'''        if (!plan)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.plan"});
        const auto& publication = *plan;
        const auto input_files = publication.files();
        std::vector<ProjectFileChange> files(input_files.begin(), input_files.end());
        files.push_back({publication.manifestPath(), publication.beforeManifestDigest(), publication.manifestBytes()});
'''+t[end:]
for field,method in {'manifest':'manifest','files':'files','root':'root','package_paths':'packagePaths'}.items():
    t=re.sub(r'publication\.'+field+r'\b(?!\()',f'publication.{method}()',t)
t=t.replace('            std::move(packages)\n','            std::move(packages),\n            std::move(plan)\n')
insert=t.index('    EditorResult<ProjectPublicationReceipt> publishProjectFiles(')
t=t[:insert]+'''    ProjectPublicationPlan::PrepareResult ProjectPublicationPlan::prepare(
        std::filesystem::path root,
        std::string manifest_path,
        std::string before_manifest_digest,
        ProjectManifest manifest,
        std::vector<ProjectFileChange> files,
        std::vector<std::string> package_paths
    )
    {
        std::error_code error;
        root = std::filesystem::absolute(root, error).lexically_normal();
        const bool is_invalid_root = error || root.filename().empty();
        const bool is_invalid_manifest = !validProjectPath(manifest_path) || before_manifest_digest.empty();
        const bool is_invalid_plan = is_invalid_root || is_invalid_manifest;
        if (is_invalid_plan)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.plan"});
        if (files.size() >= 4096)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "project.publication.files"});
        std::set<std::filesystem::path, PathLess> paths;
        paths.insert(root / std::filesystem::u8path(manifest_path));
        for (const auto& file : files)
        {
            auto target = targetPath(root, file.path);
            if (!target)
                return cxx::unexpected(std::move(target.error()));
            const bool is_duplicate = !paths.insert(*target).second;
            const bool is_invalid_version = file.before_digest.empty();
            const bool is_oversized = file.bytes.size() > file_limit;
            const bool is_invalid_file = is_duplicate || is_invalid_version || is_oversized;
            if (is_invalid_file)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.file"});
        }
        for (const auto& path : package_paths)
            if (!validProjectPath(path))
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.publication.package"});
        auto encoded = encodeProjectManifest(manifest);
        if (!encoded)
            return cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE, "project.manifest.encode", 0, {}, encoded.error()
            });
        auto bytes = std::make_shared<const std::string>(std::move(*encoded));
        auto result = std::shared_ptr<ProjectPublicationPlan>(new ProjectPublicationPlan);
        result->root_ = std::move(root);
        result->manifest_path_ = std::move(manifest_path);
        result->before_manifest_digest_ = std::move(before_manifest_digest);
        result->manifest_ = std::move(manifest);
        result->manifest_bytes_ = cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)));
        result->files_ = std::move(files);
        result->package_paths_ = std::move(package_paths);
        return std::shared_ptr<const ProjectPublicationPlan>(std::move(result));
    }

'''+t[insert:]
write(p,t)
p='editor/activities/project/src/ProjectCreation.cpp';t=read(p)
t=t.replace('EditorResult<PreparedProjectPublication> prepareProjectCreation','ProjectPublicationPlan::PrepareResult prepareProjectCreation')
t=t.replace('        PreparedProjectPublication result;\n        result.root = std::move(directory);\n        result.manifest_path = "Project.luxproject";\n        result.before_manifest_digest = "missing";\n        result.manifest = {built->project_id, std::move(built->name), {}, {}, std::move(built->plugins)};','''        ProjectManifest manifest{built->project_id, std::move(built->name), {}, {}, std::move(built->plugins)};
        std::vector<ProjectFileChange> files;''')
t=t.replace('result.manifest.','manifest.').replace('result.files.','files.')
a=t.index('        auto manifest = encodeProjectManifest(result.manifest);');b=t.index('\n    }',a)
t=t[:a]+'''        return ProjectPublicationPlan::prepare(
            std::move(directory), "Project.luxproject", "missing", std::move(manifest), std::move(files)
        );'''+t[b:]
t=t.replace('        PreparedProjectPublication& publication,','        std::shared_ptr<const ProjectPublicationPlan> plan,')
t=t.replace('        publication.root = std::filesystem::absolute(publication.root, error).lexically_normal();\n        if (error || publication.root.filename().empty())\n            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "project.create.directory"});','        const auto& publication = *plan;')
for field,method in {'root':'root','manifest_path':'manifestPath','manifest':'manifest'}.items():t=re.sub(r'publication\.'+field+r'\b',f'publication.{method}()',t)
t=t.replace('publishProjectFiles(publication, stop)','publishProjectFiles(std::move(plan), stop)')
write(p,t)
p='editor/activities/project/include/lux/engine/editor/storage/ProjectCreation.hpp';t=read(p)
t=t.replace('EditorResult<PreparedProjectPublication>','ProjectPublicationPlan::PrepareResult')
t=t.replace('            PreparedProjectPublication&,','            std::shared_ptr<const ProjectPublicationPlan>,')
t=t.replace('publishNewProject(*input, stop)','publishNewProject(std::move(*input), stop)')
write(p,t)
# Application migrates to the fixed plan now; R3 removes this implementation body.
p='editor/application/src/EditorArtifacts.cpp';t=read(p)
for var in ['candidate','entry.catalog']:
    for field,method in {'manifest':'manifest','manifest_path':'manifestPath','before_manifest_digest':'beforeManifestDigest'}.items():
        t=re.sub(re.escape(var)+r'->'+field+r'\b',f'{var}->plan().{method}()',t)
t=t.replace('                            {**entry.package}\n','                            {**entry.package},\n                            entry.catalog->sharePlan()\n')
write(p,t)
# Open preparation is an owning, private input; only the factory and Storage may consume it.
p='editor/activities/project/include/lux/engine/editor/storage/PreparedProjectOpen.hpp'
write(p,'''#pragma once

#include <lux/engine/editor/storage/ProjectPublication.hpp>

namespace lux::editor
{
    class PreparedProjectOpen final
    {
    public:
        PreparedProjectOpen(PreparedProjectOpen&&) noexcept = default;
        PreparedProjectOpen& operator=(PreparedProjectOpen&&) noexcept = default;
        PreparedProjectOpen(const PreparedProjectOpen&) = delete;
        PreparedProjectOpen& operator=(const PreparedProjectOpen&) = delete;
        [[nodiscard]] const ProjectManifest& manifest() const noexcept { return manifest_; }
        [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

    private:
        PreparedProjectOpen() = default;
        friend class ProjectStorage;
        friend EditorResult<PreparedProjectOpen> prepareProjectOpen(const std::filesystem::path&);
        ProjectWriteLease write_lease_;
        ProjectManifest manifest_;
        std::filesystem::path file_;
        std::vector<ProjectPackage> mounts_;
        std::string manifest_digest_;
        std::vector<std::pair<std::string, std::string>> source_digests_;
    };

    [[nodiscard]] LUX_EDITOR_STORAGE_PUBLIC EditorResult<PreparedProjectOpen>
    prepareProjectOpen(const std::filesystem::path&);
}
''')
fields=['manifest','file','mounts','write_lease','manifest_digest','source_digests']
p='editor/activities/project/src/PreparedProjectOpen.cpp';t=read(p)
t=t.replace('        PreparedProjectOpen result{std::move(*decoded), absolute, {}};', '        PreparedProjectOpen result;\n        result.manifest_ = std::move(*decoded);\n        result.file_ = absolute;')
for field in fields:t=re.sub(r'result\.'+field+r'\b','result.'+field+'_',t)
write(p,t)
for p in ['editor/activities/project/src/ProjectStorage.cpp','editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp']:
    t=read(p)
    for field in fields:
        for var in ['source','source_']:
            t=re.sub(r'\b'+var+r'\.'+field+r'\b',var+'.'+field+'_',t)
    write(p,t)
p='editor/activities/project/test/project_creation.cpp';t=read(p)
for var in ['opened','source']:
    for field in ['manifest','file']:t=re.sub(r'\b'+var+r'->'+field+r'\b',var+'->'+field+'()',t)
write(p,t)
