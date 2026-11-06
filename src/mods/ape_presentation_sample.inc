// Explicit source vertices -> independent camera/object poses -> sampled view
// vertices. This qualification adapter uses a retained pair of submissions;
// cadence and effective widescreen/FOV projection are separate pending gates.
struct GeometrySamples {
    std::unordered_map<uint64_t,PsxPresentationSample> vertices;
    PsxPresentationCamera camera{};
    uint32_t paired=0;
    uint32_t derived=0;
    double alpha=1;
    const char* error="unknown geometry failure";
    int16_t camera_linear[9]{};
    int16_t old_camera_linear[9]{};
    int16_t lens_linear[9]{};
};
std::unique_ptr<Frame> copy_gpu_frame(const Frame& source) {
    auto copy=std::make_unique<Frame>();*copy=source;copy->texture_version=0;
    if(!source.texture_version || !psx_presentation_gpu_retain(source.texture_version))return nullptr;
    copy->texture_version=source.texture_version;return copy;
}
bool matrix_pose(const uint32_t words[8],bool rigid,PsxPresentationPose& pose) {
    double linear[9],translation[3];
    for(unsigned i=0;i<9;++i)linear[i]=double(int16_t(words[i/2]>>((i&1)*16)))/4096.0;
    for(unsigned i=0;i<3;++i)translation[i]=int32_t(words[5+i]);
    return psx_presentation_pose_from_matrix(&pose,linear,translation,rigid)!=0;
}
bool camera_lens(const Batch& ref,double scale[3]) {
    for(unsigned i=0;i<9;++i) {
        const int16_t value=int16_t(ref.lens[i/2]>>((i&1)*16));
        if(i%4==0) {if(value<=0)return false;scale[i/4]=value/4096.0;}
        else if(value)return false;
    }
    return scale[2]==1 && !ref.lens[5] && !ref.lens[6] && !ref.lens[7];
}
const Batch* camera_reference(const Frame& frame) {
    // A DrawOTag may submit an earlier buffer while newer projections are
    // already observed. Follow actual packet lineage and derived parents;
    // unrelated projections do not select this submission's camera domain.
    std::unordered_map<uint64_t,const Batch*> sources;
    for(const auto& batch:frame.batches)sources.emplace(batch.source,&batch);
    std::unordered_set<uint64_t> required;
    std::vector<uint64_t> pending;
    for(const auto& packet:frame.packets)for(uint64_t token:packet.source)
        if(token)pending.push_back(token&~uint64_t(3));
    while(!pending.empty()) {
        const uint64_t id=pending.back();pending.pop_back();
        if(!required.insert(id).second)continue;
        const auto found=sources.find(id);
        if(found!=sources.end() && found->second->kind==Subdivision)
            for(uint64_t token:found->second->parents)if(token)pending.push_back(token&~uint64_t(3));
    }
    for(const auto& batch:frame.batches)
        if(batch.root_domain==1 && batch.lifetime && required.count(batch.source))return &batch;
    return nullptr;
}
bool frame_camera(const Batch& ref,PsxPresentationCamera& camera) {
    PsxPresentationPose world_to_view{};
    double lens[3],linear[9],translation[3];
    if(!camera_lens(ref,lens))return false;
    for(unsigned i=0;i<9;++i)
        linear[i]=double(int16_t(ref.camera[i/2]>>((i&1)*16)))/4096.0/lens[i/3];
    for(unsigned i=0;i<3;++i)translation[i]=int32_t(ref.camera[5+i])/lens[i];
    if(!psx_presentation_pose_from_matrix(&world_to_view,linear,translation,1))return false;
    const double origin[3]={0,0,0};
    if(!psx_presentation_pose_point(&world_to_view,origin,1,camera.pose.position))return false;
    camera.pose.rotation[0]=world_to_view.rotation[0];
    for(unsigned i=1;i<4;++i)camera.pose.rotation[i]=-world_to_view.rotation[i];
    for(unsigned i=0;i<3;++i)camera.pose.scale[i]=1;
    camera.focal_x=uint16_t(ref.projection[2])*lens[0];
    camera.focal_y=uint16_t(ref.projection[2])*lens[1];
    camera.center_x=int32_t(ref.projection[0])/65536.0;
    camera.center_y=int32_t(ref.projection[1])/65536.0;
    camera.near_z=std::max(1.0,camera.focal_y*.5);
    return camera.focal_x>0;
}
using VertexKey=std::tuple<uint64_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>;
struct VertexRef { const Batch* batch=nullptr;unsigned corner=0;bool ambiguous=false; };
using VertexMap=std::map<VertexKey,VertexRef>;
VertexKey vertex_key(const Batch& b,unsigned corner) {
    return {b.lifetime,b.descriptor&0x1FFFFFFFu,b.asset,b.bone,b.source_vertex[corner],b.kind};
}
VertexMap vertex_map(const Frame& f,const Batch& ref) {
    VertexMap result;
    for(const auto& b:f.batches) {
        if(b.kind==Subdivision || ((b.kind==Skeletal || b.kind==Rigid) && b.root_domain!=1) ||
            std::memcmp(b.lens,ref.lens,sizeof b.lens) ||
            std::memcmp(b.camera,ref.camera,sizeof b.camera) || std::memcmp(b.projection,ref.projection,sizeof b.projection))continue;
        for(unsigned i=0;i<b.vertex_count;++i) {
            const auto key=vertex_key(b,i);const auto found=result.find(key);
            if(found==result.end())result.emplace(key,VertexRef{&b,i,false});
            else {
                const auto& other=*found->second.batch;const unsigned c=found->second.corner;
                if(std::memcmp(b.matrix,other.matrix,sizeof b.matrix) ||
                    std::memcmp(b.world_root,other.world_root,sizeof b.world_root) ||
                    b.input[i*2]!=other.input[c*2] || b.input[i*2+1]!=other.input[c*2+1])found->second.ambiguous=true;
            }
        }
    }
    return result;
}
bool source_object(const VertexRef& source,const PsxPresentationCamera& camera,
    uint64_t identity,PsxPresentationObject& object,PsxPresentationVertex vertices[3]) {
    if(source.ambiguous)return false;
    const Batch& b=*source.batch;const unsigned corner=source.corner;
    object={};object.identity=identity;object.lifetime=b.lifetime;
    object.topology=uint64_t(b.asset)<<32 | b.bone;
    object.pose.rotation[0]=1;for(unsigned i=0;i<3;++i)object.pose.scale[i]=1;
    if(b.root_domain) {
        if(!matrix_pose(b.world_root,false,object.pose))return false;
    } else object.flags|=PSX_PRESENTATION_NATIVE;
    if(!b.lifetime)object.flags|=PSX_PRESENTATION_NATIVE;
    const int16_t input[3]={int16_t(b.input[corner*2]),int16_t(b.input[corner*2]>>16),int16_t(b.input[corner*2+1])};
    double view[3],world[3],local[3],lens[3];
    if(!camera_lens(b,lens))return false;
    for(unsigned row=0;row<3;++row) {
        view[row]=int32_t(b.matrix[5+row]);
        for(unsigned col=0;col<3;++col) {
            const unsigned index=row*3+col;
            view[row]+=double(int16_t(b.matrix[index/2]>>((index&1)*16)))*input[col]/4096.0;
        }
        view[row]/=lens[row];
    }
    // Use the explicit camera and original world root. Local-space residuals
    // retain the original bone animation and fixed-point matrix roundoff.
    if(!psx_presentation_pose_point(&camera.pose,view,0,world) ||
        !psx_presentation_pose_point(&object.pose,world,1,local))return false;
    for(unsigned i=0;i<3;++i) {
        vertices[i]={};for(unsigned k=0;k<3;++k)vertices[i].position[k]=local[k];vertices[i].rgba=0xFFFFFFFFu;
    }
    object.vertices=vertices;object.vertex_count=3;return true;
}
bool sample_derived(uint64_t token,const std::unordered_map<uint64_t,const Batch*>& sources,
    GeometrySamples& result,unsigned depth=0) {
    if(result.vertices.count(token))return true;
    if(depth>8 || (token&3))return false;
    const auto found=sources.find(token);
    if(found==sources.end() || found->second->kind!=Subdivision)return false;
    const Batch& edge=*found->second;
    for(uint64_t parent:edge.parents)if(!sample_derived(parent,sources,result,depth+1))return false;
    const auto a=result.vertices.at(edge.parents[0]),b=result.vertices.at(edge.parents[1]);
    if(a.view[2]<=0 || b.view[2]<=0)return false;
    const double za=std::max(164.0,a.view[2]),zb=std::max(164.0,b.view[2]);
    PsxPresentationSample out{};out.view[2]=(za+zb)*.5;
    for(unsigned k=0;k<2;++k)out.view[k]=(a.view[k]*za/a.view[2]+b.view[k]*zb/b.view[2])*.5;
    const double ax=a.view[0]/a.view[2],bx=b.view[0]/b.view[2];
    const double ay=a.view[1]/a.view[2],by=b.view[1]/b.view[2];
    const int bias_x=edge.edge_style==1 ? ay<by : edge.edge_style==2 ? by<ay : 0;
    const int bias_y=edge.edge_style==1 ? bx<ax : edge.edge_style==2 ? ax<bx : 0;
    out.view[0]+=bias_x*out.view[2]/result.camera.focal_x;
    out.view[1]+=bias_y*out.view[2]/result.camera.focal_y;
    out.interpolated=a.interpolated && b.interpolated;
    result.vertices.emplace(token,out);++result.derived;return true;
}
bool geometry_samples(const Frame& old_frame,const Frame& new_frame,double alpha,GeometrySamples& result) {
    const auto fail=[&](const char* reason) {result.error=reason;return false;};
    const Batch* a=camera_reference(old_frame),*b=camera_reference(new_frame);
    if(!a || !b)return fail("missing original world-camera composition binding");
    for(unsigned i=0;i<9;++i) {
        result.camera_linear[i]=int16_t(b->camera[i/2]>>((i&1)*16));
        result.old_camera_linear[i]=int16_t(a->camera[i/2]>>((i&1)*16));
        result.lens_linear[i]=int16_t(b->lens[i/2]>>((i&1)*16));
    }
    if(old_frame.serial>=new_frame.serial)return fail("source submissions not ordered");
    PsxPresentationCamera old_camera{},new_camera{};
    if(!frame_camera(*a,old_camera))return fail("old camera is not a qualified rigid transform");
    if(!frame_camera(*b,new_camera))return fail("new camera is not a qualified rigid transform");
    const auto old_vertices=vertex_map(old_frame,*a),new_vertices=vertex_map(new_frame,*b);
    if(new_vertices.empty() || new_vertices.size()>24576)return fail("no bounded vertices in camera domain");
    const uint32_t cap=uint32_t(new_vertices.size());
    using ScenePtr=std::unique_ptr<PsxPresentationScene,decltype(&psx_presentation_scene_destroy)>;
    ScenePtr old_scene(psx_presentation_scene_create(cap,cap*3,1,4),psx_presentation_scene_destroy);
    ScenePtr new_scene(psx_presentation_scene_create(cap,cap*3,1,4),psx_presentation_scene_destroy);
    if(!old_scene || !new_scene ||
        !psx_presentation_scene_begin(old_scene.get(),old_frame.epoch,old_frame.serial,&old_camera) ||
        !psx_presentation_scene_begin(new_scene.get(),new_frame.epoch,new_frame.serial,&new_camera))return fail("scene camera ingestion failed");
    const uint32_t dummy=0xFFFFFFFFu;uint32_t resource;
    if(!psx_presentation_scene_resource(old_scene.get(),&dummy,4,&resource) ||
        !psx_presentation_scene_resource(new_scene.get(),&dummy,4,&resource))return fail("scene resource ingestion failed");
    std::map<VertexKey,uint64_t> identities;uint64_t next=1;
    for(const auto& entry:new_vertices) {
        PsxPresentationObject object{};PsxPresentationVertex vertices[3];
        if(!source_object(entry.second,new_camera,next,object,vertices))continue;
        if(!psx_presentation_scene_append(new_scene.get(),&object))return fail("new source object ingestion failed");
        identities.emplace(entry.first,next++);
        const auto old=old_vertices.find(entry.first);
        if(old!=old_vertices.end() && !(object.flags&PSX_PRESENTATION_NATIVE) &&
            source_object(old->second,old_camera,object.identity,object,vertices))
            if(!psx_presentation_scene_append(old_scene.get(),&object))return fail("old source object ingestion failed");
    }
    if(identities.empty() || !psx_presentation_scene_seal(old_scene.get()) || !psx_presentation_scene_seal(new_scene.get()))return fail("no qualified sealed objects");
    std::vector<PsxPresentationSample> samples(identities.size()*3);
    if(!psx_presentation_scene_sample(old_scene.get(),new_scene.get(),alpha,&result.camera,samples.data(),uint32_t(samples.size())))return fail("geometry sample failed");
    std::unordered_map<uint64_t,PsxPresentationSample> sampled_ids;
    for(size_t i=0;i<samples.size();i+=3) {
        sampled_ids.emplace(samples[i].identity,samples[i]);if(samples[i].interpolated)++result.paired;
    }
    for(const auto& batch:new_frame.batches)if(batch.kind!=Subdivision &&
        !((batch.kind==Skeletal || batch.kind==Rigid) && batch.root_domain!=1))
        for(unsigned i=0;i<batch.vertex_count;++i) {
            const auto id=identities.find(vertex_key(batch,i));
            // Only a source admitted into the exact current camera domain can
            // receive a sample, including duplicate native material passes.
            if(id!=identities.end() && !std::memcmp(batch.camera,b->camera,sizeof batch.camera) &&
                !std::memcmp(batch.lens,b->lens,sizeof batch.lens) &&
                !std::memcmp(batch.projection,b->projection,sizeof batch.projection))
                result.vertices.emplace(batch.source+i,sampled_ids.at(id->second));
        }
    std::unordered_map<uint64_t,const Batch*> sources;
    for(const auto& batch:new_frame.batches)sources.emplace(batch.source,&batch);
    for(const auto& batch:new_frame.batches)if(batch.kind==Subdivision)sample_derived(batch.source,sources,result);
    result.alpha=alpha;return !result.vertices.empty();
}
