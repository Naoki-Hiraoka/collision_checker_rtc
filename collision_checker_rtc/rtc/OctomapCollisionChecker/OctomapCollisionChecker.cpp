// -*- C++ -*-
/*!
 * @file  OctomapCollisionChecker.cpp
 * @brief soft error limiter
 * $Date$
 *
 * $Id$
 */

#include "OctomapCollisionChecker.h"
#include <rtm/CorbaNaming.h>

#include <cnoid/BodyLoader>
#include <cnoid/SceneDrawables>
#include <cnoid/MeshExtractor>
#include <cnoid/EigenUtil>
#include <octomap_msgs/conversions.h>

// Module specification
// <rtc-template block="module_spec">
static const char* OctomapCollisionChecker_spec[] =
  {
    "implementation_id", "OctomapCollisionChecker",
    "type_name",         "OctomapCollisionChecker",
    "description",       "OctomapCollisionChecker",
    "version",           "0.0.0",
    "vendor",            "Naoki-Hiraoka",
    "category",          "example",
    "activity_type",     "DataFlowComponent",
    "max_instance",      "10",
    "language",          "C++",
    "lang_type",         "compile",
    ""
  };
// </rtc-template>

static void addMesh(cnoid::SgMeshPtr model, std::shared_ptr<cnoid::MeshExtractor> meshExtractor){
  cnoid::SgMeshPtr mesh = meshExtractor->currentMesh();
  const cnoid::Affine3& T = meshExtractor->currentTransform();

  const int vertexIndexTop = model->getOrCreateVertices()->size();

  const cnoid::SgVertexArray& vertices = *mesh->vertices();
  const int numVertices = vertices.size();
  for(int i=0; i < numVertices; ++i){
    const cnoid::Vector3 v = T * vertices[i].cast<cnoid::Affine3::Scalar>();
    model->vertices()->push_back(v.cast<cnoid::Vector3f::Scalar>());
  }

  const int numTriangles = mesh->numTriangles();
  for(int i=0; i < numTriangles; ++i){
    cnoid::SgMesh::TriangleRef tri = mesh->triangle(i);
    const int v0 = vertexIndexTop + tri[0];
    const int v1 = vertexIndexTop + tri[1];
    const int v2 = vertexIndexTop + tri[2];
    model->addTriangle(v0, v1, v2);
  }
}

static cnoid::SgMeshPtr convertToSgMesh (const cnoid::SgNodePtr collisionshape){
  if (!collisionshape) return nullptr;

  std::shared_ptr<cnoid::MeshExtractor> meshExtractor = std::make_shared<cnoid::MeshExtractor>();
  cnoid::SgMeshPtr model = new cnoid::SgMesh;
  if(meshExtractor->extract(collisionshape, [&]() { addMesh(model,meshExtractor); })){
    model->setName(collisionshape->name());
  }else{
    std::cerr << "[convertToSgMesh] meshExtractor->extract failed " << collisionshape->name() << std::endl;
    return nullptr;
  }

  return model;
}

OctomapCollisionChecker::OctomapCollisionChecker(RTC::Manager* manager)
  : RTC::DataFlowComponentBase(manager),
  m_qIn_("qIn", m_q_),
  m_basePosIn_("basePosIn", m_basePos_),
  m_baseRpyIn_("baseRpyIn", m_baseRpy_),
  m_octomapIn_("octomapIn", m_octomap_),
  m_collisionOut_("collisionOut", m_collision_)
{
}

RTC::ReturnCode_t OctomapCollisionChecker::onInitialize()
{
  std::cerr << "[" << this->m_profile.instance_name << "] onInitialize()" << std::endl;

  addInPort("qIn", this->m_qIn_);
  addInPort("basePosIn", this->m_basePosIn_);
  addInPort("baseRpyIn", this->m_baseRpyIn_);
  addInPort("octomapIn", this->m_octomapIn_);
  addOutPort("collisionOut", this->m_collisionOut_);

  // load robot model
  cnoid::BodyLoader bodyLoader;
  std::string fileName;
  if(this->getProperties().hasKey("model")) fileName = std::string(this->getProperties()["model"]);
  else fileName = std::string(this->m_pManager->getConfig()["model"]); // 引数 -o で与えたプロパティを捕捉
  std::cerr << "[" << this->m_profile.instance_name << "] model: " << fileName <<std::endl;
  this->robot_ = bodyLoader.load(fileName);
  if(!this->robot_){
    std::cerr << "\x1b[31m[" << m_profile.instance_name << "] " << "failed to load model[" << fileName << "]" << "\x1b[39m" << std::endl;
    return RTC::RTC_ERROR;
  }

  // get link vertices
  float resolution = 0.01;
  for(int i=0;i<robot_->numLinks();i++){
    cnoid::LinkPtr link = robot_->link(i);
    std::vector<cnoid::Vector3f> vertices; // 同じvertexが2回カウントされている TODO
    cnoid::SgMeshPtr mesh = convertToSgMesh(link->collisionShape());
    if(mesh) {
      for(int j=0;j<mesh->numTriangles();j++){
        cnoid::Vector3f v0 = mesh->vertices()->at(mesh->triangle(j)[0]);
        cnoid::Vector3f v1 = mesh->vertices()->at(mesh->triangle(j)[1]);
        cnoid::Vector3f v2 = mesh->vertices()->at(mesh->triangle(j)[2]);
        float l1 = (v1 - v0).norm();
        float l2 = (v2 - v0).norm();
        cnoid::Vector3f n1 = (v1 - v0).normalized();
        cnoid::Vector3f n2 = (v2 - v0).normalized();
        for(double m=0;m<l1;m+=resolution){
          for(double n=0;n<l2-l2/l1*m;n+=resolution){
            vertices.push_back(v0 + n1 * m + n2 * n);
          }
          double n=l2-l2/l1*m;
          vertices.push_back(v0 + n1 * m + n2 * n);
        }
        double m = l1;
        double n= 0;
        vertices.push_back(v0 + n1 * m + n2 * n);
      }
    }
    verticesMap_[link] = vertices;
  }

  for(int i=0;i<robot_->numLinks();i++){
    cnoid::LinkPtr link = robot_->link(i);
    if(verticesMap_[link].size() == 0) continue;
    targetLinks_.push_back(link);
  }


  return RTC::RTC_OK;
}

RTC::ReturnCode_t OctomapCollisionChecker::onExecute(RTC::UniqueId ec_id)
{
  double dt = 1.0 / this->get_context(ec_id)->get_rate();

  if (this->thread_done_ && this->thread_){
    this->thread_->join();
    this->thread_ = nullptr;
  }
  if (this->m_octomapIn_.isNew()) {
    this->m_octomapIn_.read();
    std::shared_ptr<octomap_msgs::Octomap> octomap = std::make_shared<octomap_msgs::Octomap>();
    octomap->binary = this->m_octomap_.data.octomap.binary;
    octomap->id = this->m_octomap_.data.octomap.id;
    octomap->resolution = this->m_octomap_.data.octomap.resolution;
    octomap->data.resize(this->m_octomap_.data.octomap.data.length());
    for(int i=0;i<octomap->data.size();i++) {
      octomap->data[i] = this->m_octomap_.data.octomap.data[i];
    }
    cnoid::Position fieldOrigin;
    fieldOrigin.translation()[0] = this->m_octomap_.data.origin.position.x;
    fieldOrigin.translation()[1] = this->m_octomap_.data.origin.position.y;
    fieldOrigin.translation()[2] = this->m_octomap_.data.origin.position.z;
    fieldOrigin.linear() = cnoid::rotFromRpy(this->m_octomap_.data.origin.orientation.r,this->m_octomap_.data.origin.orientation.p,this->m_octomap_.data.origin.orientation.y);
    if ( !this->thread_) {
      this->thread_done_ = false;
      this->thread_ = std::make_shared<std::thread>(&OctomapCollisionChecker::octomapCallback, this, octomap, fieldOrigin);
    }
  }

  if (this->m_qIn_.isNew()) this->m_qIn_.read();
  if (this->m_basePosIn_.isNew()) this->m_basePosIn_.read();
  if (this->m_baseRpyIn_.isNew()) this->m_baseRpyIn_.read();

  if(this->m_q_.data.length() == this->robot_->numJoints()){
    for ( int i = 0; i < this->robot_->numJoints(); i++ ){
      this->robot_->joint(i)->q() = this->m_q_.data[i];
    }
  }
  this->robot_->rootLink()->p()[0] = m_basePos_.data.x;
  this->robot_->rootLink()->p()[1] = m_basePos_.data.y;
  this->robot_->rootLink()->p()[2] = m_basePos_.data.z;
  this->robot_->rootLink()->R() = cnoid::rotFromRpy(m_baseRpy_.data.r, m_baseRpy_.data.p, m_baseRpy_.data.y);
  this->robot_->calcForwardKinematics();

  std::vector<collision_checker_msgs::CollisionIdl> collisions;

  // octomapCallbackが別スレッドで上書きしてもいいようにコピー
  std::shared_ptr<distance_field::PropagationDistanceField> field = this->field_;
  cnoid::Position fieldOrigin = this->fieldOrigin_;
  Eigen::Affine3f fieldOriginInv = fieldOrigin.inverse().cast<float>();
  if(field) {
    // update ignore bounding box
    for(int i=0;i<this->ignoreBoundingBox_.size();i++) this->ignoreBoundingBox_[i].setParentLinkPose();


    for(int i=0;i<this->targetLinks_.size();i++){
      cnoid::LinkPtr link = this->targetLinks_[i];
      Eigen::Affine3f linkT = link->T().cast<float>();

      double min_dist = this->maxDistance_ + 1;
      cnoid::Vector3f closest_v = cnoid::Vector3f::Zero();
      cnoid::Vector3 closest_point_fieldLocal = cnoid::Vector3::Zero();
      cnoid::Vector3 closest_direction_fieldLocal = cnoid::Vector3::UnitX();

      const std::vector<cnoid::Vector3f>& vertices = this->verticesMap_[link];
      for(int j=0;j<vertices.size();j++){
        cnoid::Vector3f v = linkT * vertices[j];

        bool ignore = false;
        for(int k=0;k<this->ignoreBoundingBox_.size();k++){
          if(this->ignoreBoundingBox_[k].isInside(v)) {
            ignore = true;
            break;
          }
        }
        if(ignore) continue;

        cnoid::Vector3f v_fieldLocal = fieldOriginInv * v;

        cnoid::Vector3 grad;
        bool in_bound;
        double dist = this->field_->getDistanceGradient(v_fieldLocal[0],v_fieldLocal[1],v_fieldLocal[2],grad[0],grad[1],grad[2],in_bound);
        if(in_bound && grad.norm() > 0){
          if(dist < min_dist){
            closest_direction_fieldLocal[0] = (grad[0]/grad.norm());
            closest_direction_fieldLocal[1] = (grad[1]/grad.norm());
            closest_direction_fieldLocal[2] = (grad[2]/grad.norm());
            closest_point_fieldLocal[0] = v_fieldLocal[0]-closest_direction_fieldLocal[0]*dist;
            closest_point_fieldLocal[1] = v_fieldLocal[1]-closest_direction_fieldLocal[1]*dist;
            closest_point_fieldLocal[2] = v_fieldLocal[2]-closest_direction_fieldLocal[2]*dist;
            min_dist = dist;
            closest_v = vertices[j];
          }
        }
      }

      if(min_dist <= this->maxDistance_ && min_dist >= this->minDistance_){
        cnoid::Vector3 closest_point = this->fieldOrigin_ * closest_point_fieldLocal;
        cnoid::Vector3 closest_direction = this->fieldOrigin_.linear() * closest_direction_fieldLocal;

        collision_checker_msgs::CollisionIdl collision;
        collision.link1 = link->name().c_str();
        collision.point1.x = closest_v[0];
        collision.point1.y = closest_v[1];
        collision.point1.z = closest_v[2];
        collision.link2 = "";
        collision.point2.x = closest_point[0];
        collision.point2.y = closest_point[1];
        collision.point2.z = closest_point[2];
        collision.direction21.x = closest_direction[0];
        collision.direction21.y = closest_direction[1];
        collision.direction21.z = closest_direction[2];
        collision.distance = min_dist;
        collisions.push_back(collision);
      }
    }
  }

  this->m_collision_.tm = this->m_q_.tm;
  this->m_collision_.data.length(collisions.size());
  for(size_t i=0;i<collisions.size();i++){
    this->m_collision_.data[i] = collisions[i];
  }
  this->m_collisionOut_.write();

  return RTC::RTC_OK;
}

void OctomapCollisionChecker::octomapCallback(std::shared_ptr<octomap_msgs::Octomap> octomap, cnoid::Position fieldOrigin){
  if(this->debuglevel_ >= 2.0) std::cerr << "[" << this->m_profile.instance_name << "] octomapCallback start" << std::endl;

  std::shared_ptr<octomap::AbstractOcTree> absoctree = std::shared_ptr<octomap::AbstractOcTree>(octomap_msgs::msgToMap(*octomap));
  if(!absoctree) {
    this->thread_done_ = true;
    return;
  }

  std::shared_ptr<octomap::OcTree> octree = std::dynamic_pointer_cast<octomap::OcTree>(absoctree);

  if(!octree){
    std::shared_ptr<octomap::ColorOcTree> coloroctree = std::dynamic_pointer_cast<octomap::ColorOcTree>(absoctree);
    if(coloroctree){
      std::stringstream ss;
      coloroctree->writeBinary(ss);
      octree = std::make_shared<octomap::OcTree>(absoctree->getResolution());
      if(!octree->readBinary(ss)) octree = nullptr;
    }
  }

  if(octree){
    double minx,miny,minz; octree->getMetricMin(minx,miny,minz);
    double maxx,maxy,maxz; octree->getMetricMax(maxx,maxy,maxz);
    this->field_ = std::make_shared<distance_field::PropagationDistanceField>(*octree,
                                                                              octomap::point3d(minx,miny,minz),
                                                                              octomap::point3d(maxx,maxy,maxz),
                                                                              this->maxDistance_,
                                                                              true // true: めり込み時に離れる方向を示す. 裏側に行かないよう、minDistanceをある程度大きくせよ
                                                                              );
    this->fieldOrigin_ = fieldOrigin;
  }else{
    this->field_ = nullptr;
    this->fieldOrigin_ = cnoid::Position::Identity();
  }

  this->thread_done_ = true;

  if(this->debuglevel_ >= 2.0) std::cerr << "[" << this->m_profile.instance_name << "] octomapCallback end" << std::endl;
}

extern "C"
{

  void OctomapCollisionCheckerInit(RTC::Manager* manager)
  {
    RTC::Properties profile(OctomapCollisionChecker_spec);
    manager->registerFactory(profile,
                             RTC::Create<OctomapCollisionChecker>,
                             RTC::Delete<OctomapCollisionChecker>);
  }

};

