/*
 *  Copyright 2011, 2012, 2014, DFKI GmbH Robotics Innovation Center
 *
 *  This file is part of the MARS simulation framework.
 *
 *  MARS is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation, either version 3
 *  of the License, or (at your option) any later version.
 *
 *  MARS is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with MARS.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "smurf.h"

#include <QtXml>
#include <QDomNodeList>

#include <mars/data_broker/DataBrokerInterface.h>

#include <mars/interfaces/sim/EntityManagerInterface.h>
#include <mars/interfaces/sim/SimulatorInterface.h>
#include <mars/interfaces/sim/NodeManagerInterface.h>
#include <mars/interfaces/sim/JointManagerInterface.h>
#include <mars/interfaces/sim/SensorManagerInterface.h>
#include <mars/interfaces/sim/MotorManagerInterface.h>
#include <mars/interfaces/sim/ControllerManagerInterface.h>
#include <mars/interfaces/graphics/GraphicsManagerInterface.h>
#include <mars/sim/SimEntity.h>
#include <mars/entity_generation/entity_factory/EntityFactoryManager.h>
#include <mars/interfaces/Logging.hpp>

#include <mars/utils/misc.h>
#include <mars/utils/mathUtils.h>
#include <smurf_parser/SMURFParser.h>

//#define DEBUG_PARSE_SENSOR 1

/*
 * remarks:
 *
 *   - we need some special handling because the representation
 *     in MARS is different then in URDF with is marked in the source with:
 *     ** special case handling **
 *
 *   - if we load and save a file we might lose names of
 *     collision and visual objects
 *
 */

namespace mars {
  namespace smurf {

    using namespace std;
    using namespace interfaces;
    using namespace utils;
    using namespace configmaps;

    SMURF::SMURF(lib_manager::LibManager *theManager): MarsPluginTemplate(theManager, "SMURF"),
                                                       entity_generation::EntityFactoryInterface("smurf, urdf") {
      entity_generation::EntityFactoryManager* factoryManager =
        theManager->acquireLibraryAs<mars::entity_generation::EntityFactoryManager>(
              "mars_entity_factory_manager");
      factoryManager->registerFactory("smurf", this);
      factoryManager->registerFactory("urdf", this);
    }

    SMURF::~SMURF(){
    }

    void SMURF::init() {
      control->sim->switchPluginUpdateMode(0, this);

      reset();
    }

    void SMURF::reset() {
      nextGroupID = control->nodes->getMaxGroupID() + 1;
      nextNodeID = 1;
      nextJointID = 1;
      nextMaterialID = 1;
      nextMotorID = 1;
      nextSensorID = 1;
      nextControllerID = 1;

      nodeList.clear();
      jointList.clear();
      motorList.clear();
      sensorList.clear();
      controllerList.clear();
      materialList.clear();
      lightList.clear();
      graphicList.clear();

      nodeIDMap.clear();
      jointIDMap.clear();
      sensorIDMap.clear();
      motorIDMap.clear();
      materialMap.clear();
      collisionNameMap.clear();

      robotname = "";
      model.reset();

      entityconfig.clear();
      debugMap.clear();
    }

    void SMURF::update(sReal time_ms) {
    }

    void SMURF::handleURI(ConfigMap *map, std::string uri) {
      ConfigMap map2 = ConfigMap::fromYamlFile(uri);
      handleURIs(&map2);
      map->append(map2);
    }

    void SMURF::handleURIs(ConfigMap *map) {
      if (map->find("URI") != map->end()) {
        std::string file = (std::string) (*map)["URI"][0];
        if (!file.empty() && file[0] != '/') {
          file = tmpPath + file;
        }
        handleURI(map, file);
      }
      if (map->find("URIs") != map->end()) {
        ConfigVector::iterator vIt = (*map)["URIs"].begin();
        for (; vIt != (*map)["URIs"].end(); ++vIt) {
          std::string file = (std::string) (*vIt);
          if (!file.empty() && file[0] != '/') {
            file = tmpPath + file;
          }
          handleURI(map, file);
        }
      }
    }

    void SMURF::getSensorIDList(ConfigMap *map) {
      ConfigVector::iterator it;
#warning check if objects exists in maps

      if (map->find("link") != map->end()) {
        (*map)["nodeID"] = nodeIDMap[(std::string) (*map)["link"][0]];
      }
      if (map->find("joint") != map->end()) {
        (*map)["jointID"] = jointIDMap[(std::string) (*map)["joint"][0]];
      }
      if (map->find("links") != map->end()) {
        for (it = (*map)["links"].begin(); it != (*map)["links"].end(); ++it) {
          (*map)["id"].push_back(ConfigItem(nodeIDMap[(std::string) *it]));
        }
      }
      if (map->find("joints") != map->end()) {
        for (it = (*map)["joints"].begin(); it != (*map)["joints"].end(); ++it) {
          (*map)["id"].push_back(ConfigItem(jointIDMap[(std::string) *it]));
        }
      }
      if (map->find("motors") != map->end()) {
        for (it = (*map)["motors"].begin(); it != (*map)["motors"].end(); ++it) {
          (*map)["id"].push_back(ConfigItem(motorIDMap[(std::string) *it]));
        }
      }
    }

    sim::SimEntity* SMURF::createEntity(const ConfigMap& config) {
      reset();
      sim::SimEntity* entity;
      entityconfig = config;
      std::string path = (std::string)entityconfig["path"];
      tmpPath = path;
      std::string filename = (std::string)entityconfig["URI"];
      fprintf(stderr, "SMURF::createEntity: Creating entity of type %s\n", ((std::string)entityconfig["type"]).c_str());
      if((std::string)entityconfig["type"] == "smurf") {
        model = smurf_parser::parseFile(&entityconfig, path, filename, true);
        debugMap.append(entityconfig);
        // TODO: we should have a system that first loads the URDF and then the other files in
        //   order of priority (or sort the contents in a way as to avoid errors upon loading).

        entity = new sim::SimEntity(entityconfig);
        createModel();

        ConfigMap::iterator it;
        entityconfig.toYamlFile("entityconfig.yml");
        for (it = entityconfig.begin(); it != entityconfig.end(); ++it) {
            fprintf(stderr, "  ...loading smurf data section %s.\n", it->first.c_str());
            ConfigMap tmpconfig;
            tmpconfig[it->first] = it->second;
            addConfigMap(tmpconfig);
        }
      } else { // if type is "urdf"
        std::string urdfpath = path + (std::string) entityconfig["URI"];
        fprintf(stderr, "  ...loading urdf data from %s.\n", urdfpath.c_str());
        fprintf(stderr, "parsing model: %d", parseURDF(urdfpath));
        entity = new sim::SimEntity(entityconfig);
        createModel();
      }

      // node mapping and name checking
      std::string robotname = (std::string)entityconfig["name"];
      if (robotname == "") {
              entityconfig["name"] = "blub";//(std::string)entityconfig["modelname"];
            }
      if(control->loadCenter->getMappedSceneByName(robotname) == 0) {
        control->loadCenter->setMappedSceneName(robotname);
      }
      mapIndex = control->loadCenter->getMappedSceneByName(robotname);
      fprintf(stderr, "mapIndex: %d\n", mapIndex);

      load();

      return entity;
    }

    void SMURF::addConfigMap(ConfigMap &config) {
      ConfigVector::iterator it;
      for (it = config["motors"].begin(); it != config["motors"].end(); ++it) {
        handleURIs(&it->children);
        (*it)["index"] = nextMotorID++;
        motorIDMap[(*it)["name"][0]] = nextMotorID - 1;
        (*it)["axis"] = 1;
        (*it)["jointIndex"] = jointIDMap[(*it)["joint"][0]];
        motorList.push_back((*it).children);
        debugMap["motors"] += (*it).children;
      }
      std::map<std::string, unsigned long> * idmap;
      std::map<std::string, std::string> *namemap;
      for (it = config["sensors"].begin(); it != config["sensors"].end(); ++it) {
        handleURIs(&it->children);
        ConfigMap tmpmap = it->children;
        tmpmap["attached_node"] = (ulong) nodeIDMap[(std::string) tmpmap["link"]];
        //FIXME: tmpmap["mapIndex"] = mapIndex;
        if ((std::string) tmpmap["type"] == "Joint6DOF") {
          std::string linkname = (std::string) tmpmap["link"];
          fprintf(stderr, "addConfig: %s\n", linkname.c_str());
          std::string jointname = model->getLink(linkname)->parent_joint->name;
          fprintf(stderr, "addConfig: %s\n", jointname.c_str());
          tmpmap["nodeID"] = (ulong) nodeIDMap[linkname];
          tmpmap["jointID"] = (ulong) jointIDMap[jointname];
          fprintf(stderr, "creating Joint6DOF..., %lu, %lu\n", (ulong) tmpmap["nodeID"],
              (ulong) tmpmap["jointID"]);
        }
        idmap = 0;
        namemap = 0;
        if (tmpmap.find("id") != tmpmap.end()) {
          ConfigVector tmpids;
          if (((std::string) tmpmap["type"]).find("Joint") != std::string::npos) {
            idmap = &jointIDMap;
          }
          if (((std::string) tmpmap["type"]).find("Node") != std::string::npos) {
            idmap = &nodeIDMap;
            if (((std::string) tmpmap["type"]).find("Contact") != std::string::npos)
              namemap = &collisionNameMap;
          }
          if (((std::string) tmpmap["type"]).find("Motor") != std::string::npos) {
            idmap = &motorIDMap;
          }
          for (ConfigVector::iterator idit = tmpmap["id"].begin();
              idit != tmpmap["id"].end(); ++idit) {
            if (idmap) {
              //(*idit) = (ulong)nodeIDMap[idit->getString()];
              if (namemap) {
                tmpids.push_back(ConfigItem((ulong) (*idmap)[(*namemap)[(std::string) (*idit)]]));
              } else {
                tmpids.push_back(ConfigItem((ulong) (*idmap)[(std::string) (*idit)]));
              }
            } else {
              fprintf(stderr, "Found sensor with id list, but of no known category.\n");
            }
          }
          tmpmap["id"] = tmpids;
        }
        tmpmap["index"] = nextSensorID++;
        sensorIDMap[tmpmap["name"][0]] = nextSensorID - 1;
        getSensorIDList(&tmpmap);
        sensorList.push_back(tmpmap);
        debugMap["sensors"] += tmpmap;
      }
      for (it = config["materials"].begin(); it != config["materials"].end(); ++it) {
        handleURIs(&it->children);
        std::vector<ConfigMap>::iterator mIt = materialList.begin();
        for (; mIt != materialList.end(); ++mIt) {
          if ((std::string) (*mIt)["name"][0] == (std::string) (*it)["name"][0]) {
            mIt->append(it->children);
            break;
          }
        }
      }
      for (it = config["nodes"].begin(); it != config["nodes"].end(); ++it) {
        handleURIs(&it->children);
        std::vector<ConfigMap>::iterator nIt = nodeList.begin();
        for (; nIt != nodeList.end(); ++nIt) {
          if ((std::string) (*nIt)["name"][0] == (std::string) (*it)["name"][0]) {
            ConfigMap::iterator cIt = it->children.begin();
            for (; cIt != it->children.end(); ++cIt) {
              (*nIt)[cIt->first] = cIt->second;
            }
            break;
          }
        }
      }

      for (it = config["visual"].begin(); it != config["visual"].end(); ++it) {
        handleURIs(&it->children);
        std::string cmpName = (std::string) (*it)["name"][0];
        std::vector<ConfigMap>::iterator nIt = nodeList.begin();
        if (visualNameMap.find(cmpName) != visualNameMap.end()) {
          cmpName = visualNameMap[cmpName];
          for (; nIt != nodeList.end(); ++nIt) {
            if ((std::string) (*nIt)["name"][0] == cmpName) {
              ConfigMap::iterator cIt = it->children.begin();
              for (; cIt != it->children.end(); ++cIt) {
                if (cIt->first != "name") {
                  (*nIt)[cIt->first] = cIt->second;
                }
              }
              break;
            }
          }
        }
      }

      for (it = config["collision"].begin(); it != config["collision"].end(); ++it) {
        handleURIs(&it->children);
        std::string cmpName = (std::string) (*it)["name"][0];
        std::vector<ConfigMap>::iterator nIt = nodeList.begin();
        if (collisionNameMap.find(cmpName) != collisionNameMap.end()) {
          cmpName = collisionNameMap[cmpName];
          for (; nIt != nodeList.end(); ++nIt) {
            if ((std::string) (*nIt)["name"][0] == cmpName) {
              ConfigMap::iterator cIt = it->children.begin();
              for (; cIt != it->children.end(); ++cIt) {
                if (cIt->first != "name") {
                  if (cIt->first == "bitmask") {
                    (*nIt)["coll_bitmask"] = (int) cIt->second;
                  } else {
                    (*nIt)[cIt->first] = cIt->second;
                  }
                }
              }
              break;
            }
          }
        }
      }

      for (it = config["lights"].begin(); it != config["lights"].end(); ++it) {
        handleURIs(&it->children);
        lightList.push_back((*it).children);
        debugMap["lights"] += (*it).children;
      }
      for (it = config["graphics"].begin(); it != config["graphics"].end(); ++it) {
        handleURIs(&it->children);
        graphicList.push_back((*it).children);
        debugMap["graphics"] += (*it).children;
      }
      for (it = config["controllers"].begin(); it != config["controllers"].end(); ++it) {
        handleURIs(&it->children);
        (*it)["index"] = nextControllerID++;
        // convert names to ids
        ConfigVector::iterator it2;
        if (it->children.find("sensors") != it->children.end()) {
          for (it2 = it->children["sensors"].begin(); it2 != it->children["sensors"].end(); ++it2) {
            it->children["sensorid"].push_back(ConfigItem(sensorIDMap[(std::string) *it2]));
          }
        }
        if (it->children.find("motors") != it->children.end()) {
          for (it2 = it->children["motors"].begin(); it2 != it->children["motors"].end(); ++it2) {
            it->children["motorid"] += ConfigItem(motorIDMap[(std::string) *it2]);
          }
        }
        controllerList.push_back((*it).children);
        debugMap["controllers"] += (*it).children;
      }
    }

    void SMURF::handleInertial(ConfigMap *map, const boost::shared_ptr<urdf::Link> &link) {
      if (link->inertial) {
        (*map)["density"] = 0.0;
        (*map)["mass"] = link->inertial->mass;
        // handle inertial
        (*map)["i00"] = link->inertial->ixx;
        (*map)["i01"] = link->inertial->ixy;
        (*map)["i02"] = link->inertial->ixz;
        (*map)["i10"] = link->inertial->ixy;
        (*map)["i11"] = link->inertial->iyy;
        (*map)["i12"] = link->inertial->iyz;
        (*map)["i20"] = link->inertial->ixz;
        (*map)["i21"] = link->inertial->iyz;
        (*map)["i22"] = link->inertial->izz;
        (*map)["inertia"] = true;
      } else {
        (*map)["inertia"] = false;
      }
    }

    void SMURF::calculatePose(ConfigMap *map, const boost::shared_ptr<urdf::Link> &link) {
      urdf::Pose jointPose, parentInertialPose, inertialPose;
      urdf::Pose goalPose;

      if (link->parent_joint) {
        jointPose = link->parent_joint->parent_to_joint_origin_transform;
        if (link->getParent()->inertial) {
          parentInertialPose = link->getParent()->inertial->origin;
        } else if (link->getParent()->collision) {
          parentInertialPose = link->getParent()->collision->origin;
        }
        unsigned long parentID = nodeIDMap[link->getParent()->name];
        (*map)["relativeid"] = parentID;
      } else {
        (*map)["relativeid"] = 0ul;
      }

      if (link->inertial) {
        inertialPose = link->inertial->origin;
      }
      /** special case handling **/
      else if (link->collision) {
        // if we don't have an inertial but a collision (standard for MARS)
        // we place the node at the position of the collision
        inertialPose = link->collision->origin;
      }
      // we need the inverse of parentInertialPose.position
      parentInertialPose.position.x *= -1;
      parentInertialPose.position.y *= -1;
      parentInertialPose.position.z *= -1;

      goalPose.position = jointPose.position + parentInertialPose.position;
      goalPose.position = (goalPose.position + jointPose.rotation * inertialPose.position);
      goalPose.position = (parentInertialPose.rotation.GetInverse() * goalPose.position);

      goalPose.rotation = (parentInertialPose.rotation.GetInverse() * jointPose.rotation
          * inertialPose.rotation);

//      goalPose.rotation = (jointPose.rotation*inertialPose.rotation*
//                           parentInertialPose.rotation.GetInverse());

      Vector v(goalPose.position.x, goalPose.position.y, goalPose.position.z);
      vectorToConfigItem(&(*map)["position"][0], &v);
      Quaternion q = quaternionFromMembers(goalPose.rotation);
      quaternionToConfigItem(&(*map)["rotation"][0], &q);
    }

    urdf::Pose SMURF::getGlobalPose(const boost::shared_ptr<urdf::Link> &link) {
      urdf::Pose globalPose;
      boost::shared_ptr<urdf::Link> pLink = link->getParent();
      if (link->parent_joint) {
        globalPose = link->parent_joint->parent_to_joint_origin_transform;
      }
      if (pLink) {
        urdf::Pose parentPose = getGlobalPose(pLink);
        globalPose.position = parentPose.rotation * globalPose.position;
        globalPose.position = globalPose.position + parentPose.position;
        globalPose.rotation = parentPose.rotation * globalPose.rotation;
      }
      return globalPose;
    }

    void SMURF::handleVisual(ConfigMap *map, const boost::shared_ptr<urdf::Visual> &visual) {
      boost::shared_ptr<urdf::Geometry> tmpGeometry = visual->geometry;
      Vector size(0.0, 0.0, 0.0);
      Vector scale(1.0, 1.0, 1.0);
      urdf::Vector3 v;
      (*map)["filename"] = "PRIMITIVE";
      switch (tmpGeometry->type) {
      case urdf::Geometry::SPHERE:
        size.x() = ((urdf::Sphere*) tmpGeometry.get())->radius;
        (*map)["origname"] = "sphere";
        break;
      case urdf::Geometry::BOX:
        v = ((urdf::Box*) tmpGeometry.get())->dim;
        size = Vector(v.x, v.y, v.z);
        (*map)["origname"] = "box";
        break;
      case urdf::Geometry::CYLINDER:
        size.x() = ((urdf::Cylinder*) tmpGeometry.get())->radius;
        size.y() = ((urdf::Cylinder*) tmpGeometry.get())->length;
        (*map)["origname"] = "cylinder";
        break;
      case urdf::Geometry::MESH:
        v = ((urdf::Mesh*) tmpGeometry.get())->scale;
        scale = Vector(v.x, v.y, v.z);
        (*map)["filename"] = ((urdf::Mesh*) tmpGeometry.get())->filename;
        (*map)["origname"] = "";
        break;
      default:
        break;
      }
      vectorToConfigItem(&(*map)["visualsize"][0], &size);
      vectorToConfigItem(&(*map)["visualscale"][0], &scale);
      (*map)["materialName"] = visual->material_name;
    }

    void SMURF::convertPose(const urdf::Pose &pose, const boost::shared_ptr<urdf::Link> &link,
        Vector *v, Quaternion *q) {
      urdf::Pose toPose;

      if (link->inertial) {
        toPose = link->inertial->origin;
      }
      /** special case handling **/
      else if (link->collision) {
        // if we don't have an inertial but a collision (standard for MARS)
        // we place the node at the position of the collision
        toPose = link->collision->origin;
      }

      convertPose(pose, toPose, v, q);
    }

    void SMURF::convertPose(const urdf::Pose &pose, const urdf::Pose &toPose, Vector *v,
        Quaternion *q) {
      urdf::Pose pose_ = pose;
      urdf::Pose toPose_ = toPose;
      urdf::Vector3 p;
      urdf::Rotation r;

      // we need the inverse of toPose_.position
      toPose_.position.x *= -1;
      toPose_.position.y *= -1;
      toPose_.position.z *= -1;
      p = pose_.position + toPose_.position;
      p = toPose_.rotation.GetInverse() * p;
      r = (toPose_.rotation.GetInverse() * pose_.rotation);
      *v = Vector(p.x, p.y, p.z);
      *q = quaternionFromMembers(r);
    }

    bool SMURF::isEqualPos(const urdf::Pose &p1, const urdf::Pose p2) {
      bool equal = true;
      double epsilon = 0.00000000001;
      if (fabs(p1.position.x - p2.position.x) > epsilon)
        equal = false;
      if (fabs(p1.position.y - p2.position.y) > epsilon)
        equal = false;
      if (fabs(p1.position.z - p2.position.z) > epsilon)
        equal = false;
      if (fabs(p1.rotation.x - p2.rotation.x) > epsilon)
        equal = false;
      if (fabs(p1.rotation.y - p2.rotation.y) > epsilon)
        equal = false;
      if (fabs(p1.rotation.z - p2.rotation.z) > epsilon)
        equal = false;
      if (fabs(p1.rotation.w - p2.rotation.w) > epsilon)
        equal = false;
      return equal;
    }

    void SMURF::handleCollision(ConfigMap *map, const boost::shared_ptr<urdf::Collision> &c) {
      boost::shared_ptr<urdf::Geometry> tmpGeometry = c->geometry;
      Vector size(0.0, 0.0, 0.0);
      Vector scale(1.0, 1.0, 1.0);
      urdf::Vector3 v;
      switch (tmpGeometry->type) {
      case urdf::Geometry::SPHERE:
        size.x() = ((urdf::Sphere*) tmpGeometry.get())->radius;
        (*map)["physicmode"] = "sphere";
        break;
      case urdf::Geometry::BOX:
        v = ((urdf::Box*) tmpGeometry.get())->dim;
        size = Vector(v.x, v.y, v.z);
        (*map)["physicmode"] = "box";
        break;
      case urdf::Geometry::CYLINDER:
        size.x() = ((urdf::Cylinder*) tmpGeometry.get())->radius;
        size.y() = ((urdf::Cylinder*) tmpGeometry.get())->length;
        (*map)["physicmode"] = "cylinder";
        break;
      case urdf::Geometry::MESH:
        v = ((urdf::Mesh*) tmpGeometry.get())->scale;
        scale = Vector(v.x, v.y, v.z);
        (*map)["filename"] = ((urdf::Mesh*) tmpGeometry.get())->filename;
        (*map)["origname"] = "";
        (*map)["physicmode"] = "mesh";
        break;
      default:
        break;
      }
      vectorToConfigItem(&(*map)["extend"][0], &size);
      vectorToConfigItem(&(*map)["scale"][0], &scale);
      // todo: we need to deal correctly with the scale and size in MARS
      //       if we have a mesh here, as a first hack we use the scale as size
      if (tmpGeometry->type == urdf::Geometry::MESH) {
        vectorToConfigItem(&(*map)["extend"][0], &scale);
      }
    }

    void SMURF::createFakeMaterial() {
      ConfigMap config;

      config["id"] = nextMaterialID++;
      config["name"] = "_fakeMaterial";
      config["exists"] = true;
      config["diffuseFront"][0]["a"] = 1.0;
      config["diffuseFront"][0]["r"] = 1.0;
      config["diffuseFront"][0]["g"] = 0.0;
      config["diffuseFront"][0]["b"] = 0.0;
      config["texturename"] = "";
      config["cullMask"] = 1;
      debugMap["materials"] += config;
      materialList.push_back(config);
    }

    void SMURF::createFakeVisual(ConfigMap *map) {
      Vector size(0.01, 0.01, 0.01);
      Vector scale(1.0, 1.0, 1.0);
      (*map)["filename"] = "PRIMITIVE";
      (*map)["origname"] = "box";
      (*map)["materialName"] = "_fakeMaterial";
      (*map)["movable"] = true;
      vectorToConfigItem(&(*map)["visualsize"][0], &size);
      vectorToConfigItem(&(*map)["visualscale"][0], &scale);
    }

    void SMURF::createFakeCollision(ConfigMap *map) {
      Vector size(0.01, 0.01, 0.01);
      (*map)["physicmode"] = "box";
      (*map)["coll_bitmask"] = 0;
      (*map)["movable"] = true;
      vectorToConfigItem(&(*map)["extend"][0], &size);
    }

    void SMURF::handleKinematics(boost::shared_ptr<urdf::Link> link) {
      ConfigMap config;
      // holds the index of the next visual object to load
      size_t visualArrayIndex = 0;
      // holds the index of the next collision object to load
      size_t collisionArrayIndex = 0;
      bool loadVisual = link->visual;
      bool loadCollision = link->collision;
      Vector v;
      Quaternion q;

      config["name"] = link->name;
      config["index"] = nextNodeID++;

      nodeIDMap[link->name] = nextNodeID - 1;

      // todo: if we don't have any joints connected we need some more
      //       special handling and change the handling below
      //       config["movable"] ?!?
      // TODO: we should also read materials from the visual object here, as URDF does not
      //         necessarily define them on the top level of the file

      config["movable"] = true;

      // we do most of the special case handling here:
      { /** special case handling **/
        bool needGroupID = false;
        if (link->visual_array.size() > 1 || link->collision_array.size() > 1) {
          needGroupID = true;
        }
        if (link->collision && link->inertial) {
          if (!isEqualPos(link->collision->origin, link->inertial->origin)) {
            loadCollision = false;
            needGroupID = true;
          }
        }
        if (link->visual && link->collision) {
          if (loadCollision && link->collision->geometry->type == urdf::Geometry::MESH) {
            if (link->visual->geometry->type != urdf::Geometry::MESH) {
              loadVisual = false;
              needGroupID = true;
            } else {
              if (((urdf::Mesh*) link->collision->geometry.get())->filename
                  != ((urdf::Mesh*) link->visual->geometry.get())->filename) {
                loadVisual = false;
                needGroupID = true;
              }
            }
          }
        }
        if (needGroupID) {
          // we need to group mars nodes
          config["groupid"] = nextGroupID++;
        } else {
          config["groupid"] = 0;
        }
      }

      // we always handle the inertial
      handleInertial(&config, link);

      // calculates the pose including all case handling
      calculatePose(&config, link);

      if (loadVisual) {
        visualNameMap[link->visual->name] = link->name;
        handleVisual(&config, link->visual);
        // caculate visual position offset
        convertPose(link->visual->origin, link, &v, &q);
        vectorToConfigItem(&config["visualposition"][0], &v);
        quaternionToConfigItem(&config["visualrotation"][0], &q);
        // the first visual object is SMURFed
        visualArrayIndex = 1;
      } else {
        // we need a fake visual for the node
        createFakeVisual(&config);
      }

      if (loadCollision) {
        collisionNameMap[link->collision->name] = link->name;
        handleCollision(&config, link->collision);
        // the first visual object is SMURFed
        collisionArrayIndex = 1;
      } else {
        createFakeCollision(&config);
      }

      debugMap["links"] += config;
      nodeList.push_back(config);

      // now we have all information for the main node and can create additional
      // nodes for the collision and visual array
      while (collisionArrayIndex < link->collision_array.size()) {
        ConfigMap childNode;
        boost::shared_ptr<urdf::Collision> collision;
        boost::shared_ptr<urdf::Visual> visual;
        collision = link->collision_array[collisionArrayIndex];
        if (visualArrayIndex < link->visual_array.size()) {
          visual = link->visual_array[visualArrayIndex];
          // check wether we can SMURF visual and collision together
          /** special case handling **/
          if (collision->geometry->type == urdf::Geometry::MESH) {
            if (visual->geometry->type != urdf::Geometry::MESH) {
              visual.reset();
            } else if (((urdf::Mesh*) collision->geometry.get())->filename
                != ((urdf::Mesh*) visual->geometry.get())->filename) {
              visual.reset();
            }
          }
        }

        childNode["index"] = nextNodeID++;
        std::string childNodeName = ((std::string) config["name"][0]) + "_child";
        nodeIDMap[childNodeName] = nextNodeID - 1;
        nodeIDMap[collision->name] = nextNodeID - 1; //FIXME: can we simply duplicate this?
        childNode["relativeid"] = config["index"];
        if (collision->name.empty()) {
          childNode["name"] = childNodeName;
        } else {
          childNode["name"] = collision->name;
        }
        // add nodes created with collision names to name map
        collisionNameMap[collision->name] = collision->name;
        childNode["groupid"] = config["groupid"];
        // we add a collision node without mass
        childNode["mass"] = 0.001;
        childNode["density"] = 0.0;
        childNode["movable"] = true;

        handleCollision(&childNode, collision);
        convertPose(collision->origin, link, &v, &q);
        vectorToConfigItem(&childNode["position"][0], &v);
        quaternionToConfigItem(&childNode["rotation"][0], &q);
        urdf::Pose p1;
        p1.position = urdf::Vector3(v.x(), v.y(), v.z());
        p1.rotation = urdf::Rotation(q.x(), q.y(), q.z(), q.w());
        collisionArrayIndex++;

        if (visual) {
          visualNameMap[visual->name] = link->name;
          handleVisual(&childNode, visual);
          // convert the pose into the same coordinate system like as the node
          convertPose(visual->origin, link, &v, &q);
          urdf::Pose p2;
          p2.position = urdf::Vector3(v.x(), v.y(), v.z());
          p2.rotation = urdf::Rotation(q.x(), q.y(), q.z(), q.w());
          // then create the relative from node pose to visual pose
          convertPose(p2, p1, &v, &q);
          vectorToConfigItem(&childNode["visualposition"][0], &v);
          quaternionToConfigItem(&childNode["visualrotation"][0], &q);
          visualArrayIndex++;
        } else {
          createFakeVisual(&childNode);
        }
        debugMap["childNodes"] += childNode;
        nodeList.push_back(childNode);
      }

      while (visualArrayIndex < link->visual_array.size()) {
        ConfigMap childNode;
        boost::shared_ptr<urdf::Visual> visual;
        visual = link->visual_array[visualArrayIndex];

        childNode["index"] = nextNodeID++;
        childNode["relativeid"] = config["index"];
        if (visual->name.empty()) {
          childNode["name"] = ((std::string) config["name"][0]) + "_child";
        } else {
          childNode["name"] = visual->name;
          visualNameMap[visual->name] = visual->name;
        }
        childNode["groupid"] = config["groupid"];
        childNode["noPhysical"] = false;
        childNode["mass"] = 0.001;
        childNode["density"] = 0.0;
        childNode["movable"] = true;
        childNode["coll_bitmask"] = 0;

        handleVisual(&childNode, visual);
        childNode["physicmode"] = "box";

        Vector v(0.001, 0.001, 0.001);
        vectorToConfigItem(&childNode["extend"][0], &v);
        convertPose(visual->origin, link, &v, &q);
        vectorToConfigItem(&childNode["position"][0], &v);
        quaternionToConfigItem(&childNode["rotation"][0], &q);
        visualArrayIndex++;
        debugMap["childNodes"] += childNode;
        nodeList.push_back(childNode);
      }

      // TODO:  complete handle joint information
      if (link->parent_joint) {
        ConfigMap joint;
        joint["name"] = link->parent_joint->name;
        joint["index"] = nextJointID++;
        jointIDMap[link->parent_joint->name] = nextJointID - 1;
        joint["nodeindex1"] = nodeIDMap[link->parent_joint->parent_link_name];
        joint["nodeindex2"] = nodeIDMap[link->parent_joint->child_link_name];
        joint["anchorpos"] = ANCHOR_CUSTOM;
        if (link->parent_joint->limits) {
          joint["lowStopAxis1"] = link->parent_joint->limits->lower;
          joint["highStopAxis1"] = link->parent_joint->limits->upper;
        }
        // FIXME: we do not at this point read the joint "maxeffort" and "maxvelocity"
        // limits as they are effectively motor values and should be used only
        // if there are no explicit motor values defined
        if (link->parent_joint->type == urdf::Joint::REVOLUTE
            || link->parent_joint->type == urdf::Joint::CONTINUOUS) {
          joint["type"] = "hinge";
        } else if (link->parent_joint->type == urdf::Joint::PRISMATIC) {
          joint["type"] = "slider";
        } else if (link->parent_joint->type == urdf::Joint::FIXED) {
          joint["type"] = "fixed";
        } else {
          // we don't support the type yet and use a fixed joint
          joint["type"] = "fixed";
        }

        urdf::Pose pose = getGlobalPose(link);
        urdf::Pose pose2;
        pose2.position = pose.rotation * link->parent_joint->axis;
        v = Vector(pose2.position.x, pose2.position.y, pose2.position.z);
        vectorToConfigItem(&joint["axis1"][0], &v);

        v = Vector(pose.position.x, pose.position.y, pose.position.z);
        vectorToConfigItem(&joint["anchor"][0], &v);

        debugMap["joints"] += joint;
        jointList.push_back(joint);
      }

      for (std::vector<boost::shared_ptr<urdf::Link> >::iterator it = link->child_links.begin();
          it != link->child_links.end(); ++it) {
        handleKinematics(*it); //TODO: check if this is correct with shared_ptr
      }
    }

    void SMURF::handleMaterial(boost::shared_ptr<urdf::Material> material) {
      ConfigMap config;

      config["id"] = nextMaterialID++;
      config["name"] = material->name;
      config["exists"] = true;
      config["diffuseFront"][0]["a"] = (double) material->color.a;
      config["diffuseFront"][0]["r"] = (double) material->color.r;
      config["diffuseFront"][0]["g"] = (double) material->color.g;
      config["diffuseFront"][0]["b"] = (double) material->color.b;
      config["texturename"] = material->texture_filename;
      debugMap["materials"] += config;
      materialList.push_back(config);
    }

    unsigned int SMURF::parseURDF(std::string filename) {
      QString xmlErrorMsg = "";

      //creating a handle for the xmlfile
      QFile file(filename.c_str());

      QLocale::setDefault(QLocale::C);

      LOG_INFO("SMURF: smurfing scene: %s", filename.c_str());

      //test to open the xmlfile
      if (!file.open(QIODevice::ReadOnly)) {
        std::cout << "Error while opening scene file content " << filename
            << " in SMURF.cpp->parseScene" << std::endl;
        std::cout << "Make sure your scenefile name corresponds to"
            << " the name given to the enclosed .scene file" << std::endl;
        return 0;
      }

      model = urdf::parseURDFFile(filename);
      if (!model) {
        return 0;
      }
    }

    void SMURF::createModel() {

      if (robotname == "") {
        robotname = model.get()->name_;
      }

      createFakeMaterial();
      std::map<std::string, boost::shared_ptr<urdf::Material> >::iterator it;
      for (it = model->materials_.begin(); it != model->materials_.end(); ++it) {
        handleMaterial(it->second);
      }

      handleKinematics(model->root_link_);

      //    //the entire tree recursively anyway
      //    std::vector<boost::shared_ptr<urdf::Link>> urdflinklist;
      //    std::vector<boost::shared_ptr<urdf::Joint>> urdfjointlist;
      //

      //    model.getJoints(urdfjointlist);
      //    for (std::vector<boost::shared_ptr<urdf::Link>>::iterator it =
      //            urdfjointlist.begin(); it != urdfjointlist.end(); ++it) {
      //        getGenericConfig(&jointList, it);
      //    }
    }

    unsigned int SMURF::load() {
      fprintf(stderr, "smurfing robot: %s...\n", robotname.c_str());
      debugMap.toYamlFile("debugMap.yml");

      for (unsigned int i = 0; i < materialList.size(); ++i)
        if (!loadMaterial(materialList[i]))
          return 0;
      for (unsigned int i = 0; i < nodeList.size(); ++i)
        if (!loadNode(nodeList[i]))
          return 0;

      for (unsigned int i = 0; i < jointList.size(); ++i)
        if (!loadJoint(jointList[i]))
          return 0;

      for (unsigned int i = 0; i < motorList.size(); ++i)
        if (!loadMotor(motorList[i]))
          return 0;

      for (unsigned int i = 0; i < sensorList.size(); ++i)
        if (!loadSensor(sensorList[i]))
          return 0;

      for (unsigned int i = 0; i < controllerList.size(); ++i)
        if (!loadController(controllerList[i]))
          return 0;

      for (unsigned int i = 0; i < lightList.size(); ++i)
        if (!loadLight(lightList[i]))
          return 0;

      for (unsigned int i = 0; i < graphicList.size(); ++i)
        if (!loadGraphic(graphicList[i]))
          return 0;

      setPose();

      return 1;
    }

    void SMURF::setPose() {
      core_objects_exchange node;
      uint nodeid = control->loadCenter->getMappedID(nodeIDMap[model->root_link_->name],
          MAP_TYPE_NODE, mapIndex);
      fprintf(stderr, "placing: %s, %d\n", (model->root_link_->name).c_str(), nodeid);
      control->nodes->getNodeExchange(nodeid, &node);
      Quaternion tmpQ;
      Vector tmpR;
      Vector tmpV;
      tmpV[0] = entityconfig["position"][0];
      tmpV[1] = entityconfig["position"][1];
      tmpV[2] = entityconfig["position"][2];
      fprintf(stderr, "entityposition_x: %f\n", (double) entityconfig["position"][0]);
      fprintf(stderr, "entityposition_y: %f\n", (double) entityconfig["position"][1]);
      fprintf(stderr, "entityposition_z: %f\n", (double) entityconfig["position"][2]);
      tmpR[0] = entityconfig["rotation"][0];
      tmpR[1] = entityconfig["rotation"][1];
      tmpR[2] = entityconfig["rotation"][2];
      tmpQ = eulerToQuaternion(tmpR);

      NodeData my_node;
      my_node.index = nodeid;
      my_node.pos = tmpV;
      my_node.rot = tmpQ;
      control->nodes->editNode(&my_node, EDIT_NODE_POS | EDIT_NODE_MOVE_ALL);
      control->nodes->editNode(&my_node, EDIT_NODE_ROT | EDIT_NODE_MOVE_ALL);
    }

    unsigned int SMURF::loadNode(ConfigMap config) {
      NodeData node;
      config["mapIndex"].push_back(ConfigItem(mapIndex));
      int valid = node.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid)
        return 0;

      if ((std::string) config["materialName"][0] != std::string("")) {
        std::map<std::string, MaterialData>::iterator it;
        it = materialMap.find(config["materialName"][0]);
        if (it != materialMap.end()) {
          node.material = it->second;
        }
      } else {
        node.material.diffuseFront = Color(0.4, 0.4, 0.4, 1.0);
      }

      // check if meshes are stored as `.stl` file
      string suffix = getFilenameSuffix(node.filename);
      if (suffix == ".stl" || suffix == ".STL") {
        // add an additional rotation of -90.0 degree due to wrong definition
        // of which direction is up within .stl (for .stl -Y is up and in MARS
        // Z is up)
        node.visual_offset_rot *= eulerToQuaternion(Vector(-90.0, 0.0, 0.0));
      }

      NodeId oldId = node.index;
      config.toYamlFile("SMURFNode.yml");
      NodeId newId = control->nodes->addNode(&node);
      if (!newId) {
        LOG_ERROR("addNode returned 0");
        return 0;
      }
      control->loadCenter->setMappedID(oldId, newId, MAP_TYPE_NODE, mapIndex);
      if (robotname != "") {
        control->entities->addNode(robotname, node.index, node.name);
      }
      return 1;
    }

    unsigned int SMURF::loadMaterial(ConfigMap config) {
      MaterialData material;

      int valid = material.fromConfigMap(&config, tmpPath);
      materialMap[config["name"][0]] = material;

      return valid;
    }

    unsigned int SMURF::loadJoint(ConfigMap config) {
      JointData joint;
      joint.invertAxis = true;
      config["mapIndex"].push_back(ConfigItem(mapIndex));
      int valid = joint.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid) {
        fprintf(stderr, "SMURF: error while smurfing joint\n");
        return 0;
      }

      JointId oldId = joint.index;
      JointId newId = control->joints->addJoint(&joint);
      if (!newId) {
        LOG_ERROR("addJoint returned 0");
        return 0;
      }
      control->loadCenter->setMappedID(oldId, newId, MAP_TYPE_JOINT, mapIndex);

      if (robotname != "") {
        control->entities->addJoint(robotname, joint.index, joint.name);
      }
      return true;
    }

    unsigned int SMURF::loadMotor(ConfigMap config) {
      MotorData motor;
      config["mapIndex"].push_back(ConfigItem(mapIndex));

      int valid = motor.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid) {
        fprintf(stderr, "SMURF: error while smurfing motor\n");
        return 0;
      }

      MotorId oldId = motor.index;
      MotorId newId = control->motors->addMotor(&motor);
      if (!newId) {
        LOG_ERROR("addMotor returned 0");
        return 0;
      }
      control->loadCenter->setMappedID(oldId, newId, MAP_TYPE_MOTOR, mapIndex);

      if (robotname != "") {
        control->entities->addMotor(robotname, motor.index, motor.name);
      }
      return true;
    }

    BaseSensor* SMURF::loadSensor(ConfigMap config) {
      config["mapIndex"].push_back(ConfigItem(mapIndex));
//      fprintf(stderr, "creating sensor: %s, %s", ((std::string)config["name"]).c_str(),
//          ((std::string)config["type"]).c_str());
      BaseSensor *sensor = control->sensors->createAndAddSensor(&config);
      if (sensor != 0) {
        control->loadCenter->setMappedID((ulong) config["index"], sensor->getID(), MAP_TYPE_SENSOR,
            mapIndex);
      }

      return sensor;
    }

    unsigned int SMURF::loadGraphic(ConfigMap config) {
      GraphicData graphic;
      config["mapIndex"].push_back(ConfigItem(mapIndex));
      int valid = graphic.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid) {
        fprintf(stderr, "SMURF: error while smurfing graphic\n");
        return 0;
      }

      if (control->graphics)
        control->graphics->setGraphicOptions(graphic);

      return 1;
    }

    unsigned int SMURF::loadLight(ConfigMap config) {
      LightData light;
      config["mapIndex"].push_back(ConfigItem(mapIndex));
      int valid = light.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid) {
        fprintf(stderr, "SMURF: error while smurfing light\n");
        return 0;
      }
      control->sim->addLight(light);
      return true;
    }

    unsigned int SMURF::loadController(ConfigMap config) {
      ControllerData controller;
      config["mapIndex"].push_back(ConfigItem(mapIndex));

      int valid = controller.fromConfigMap(&config, tmpPath, control->loadCenter);
      if (!valid) {
        fprintf(stderr, "SMURF: error while smurfing Controller\n");
        return 0;
      }

      MotorId oldId = controller.id;
      MotorId newId = control->controllers->addController(controller);
      if (!newId) {
        LOG_ERROR("SMURF: addController returned 0");
        return 0;
      }
      control->loadCenter->setMappedID(oldId, newId, MAP_TYPE_CONTROLLER, mapIndex);
      if (robotname != "") {
        control->entities->addController(robotname, newId);
      }
      return 1;
    }

    std::string SMURF::getRobotname() {
      return robotname;
    }

  }        // end of namespace smurf
}
// end of namespace mars

DESTROY_LIB(mars::smurf::SMURF);
CREATE_LIB(mars::smurf::SMURF);
