#include <stdlib.h>
#include <ostream>
#include <stdio.h>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <vector>
#include "ceres/ceres.h"
#include "ceres/rotation.h"
#include <iostream>
#include <ros/ros.h>
#include <industrial_extrinsic_cal/basic_types.h>
#include <boost/foreach.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/linear_congruential.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/uniform_real.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/generator_iterator.hpp>

using std::ifstream;
using std::string;
using std::vector;
using industrial_extrinsic_cal::Point3d;
using industrial_extrinsic_cal::Pose6d;
using industrial_extrinsic_cal::P_BLOCK;

typedef boost::minstd_rand base_gen_type;
base_gen_type gen(42);
boost::normal_distribution<> normal_dist(0,1); // zero mean unit variance
boost::variate_generator<base_gen_type&, boost::normal_distribution<> > randn(gen, normal_dist);

typedef struct
{
  double x; // image x
  double y; // image y
} Observation;


typedef struct
{
  union
  {
    struct
    {
      double PB_extrinsics[6]; // parameter block for intrinsics
      double PB_intrinsics[4]; // parameter block for extrinsics
    };
    struct
    {
      double angle_axis[3];	// angle axis data
      double position[3];	// position data
      double focal_length_x;	// focal length x
      double focal_length_y;	// focal length y
      double center_x;		// center x
      double center_y;		// center y
    };
  };
  string camera_name;
  int height;
  int width;
  vector<Pose6d> pose_history;
  double pose_position_sigma;
  double pose_orientation_sigma;
  int num_observations;
} Camera;

class ObservationDataPoint{
public:
  ObservationDataPoint(Camera* camera, int point_id, P_BLOCK p_position, Observation obs)
  {
    camera_ = camera;
    point_id_ = point_id;
    point_position_ = p_position;
    image_loc_ = obs;
  }
  ;

  ~ObservationDataPoint()
  {
  }
  ;

  Camera* camera_;
  int point_id_;
  P_BLOCK camera_extrinsics_;
  P_BLOCK point_position_;
  Observation image_loc_;
};
// end of class ObservationDataPoint


// local prototypes
void print_QTasH(double qx, double qy, double qz, double qw, double tx, double ty, double tz);
void print_AATasH(double x, double y, double z, double tx, double ty, double tz);
void print_AATasHI(double x, double y, double z, double tx, double ty, double tz);
void print_AAasEuler(double x, double y, double z);
void print_camera(Camera C, string words);

Observation project_point_no_distortion(Camera C, Point3d P);
void compute_observations(vector<Camera> &cameras, 
			  vector<Point3d> &points, 
			  double noise,
			  double max_dist,
			  vector<ObservationDataPoint> &observations);
void perturb_cameras(vector<Camera> &cameras, double position_noise, double degrees_noise);
void copy_cameras_wo_history(vector<Camera> &original_cameras, vector<Camera> & cameras);
void copy_points(vector<Point3d> &original_points, vector<Point3d> & points);
void add_pose_to_history(vector<Camera> &cameras, vector<Camera> &original_cameras);
void compute_historic_pose_statistics(vector<Camera> &cameras);
void parse_points(ifstream & points_input_file,vector<Point3d> &original_points);
void parse_cameras(ifstream & cameras_input_file,vector<Camera> & original_cameras);
void compare_cameras(vector<Camera> &C1, vector<Camera> &C2);
void compare_observations(vector<ObservationDataPoint> &O1, vector<ObservationDataPoint> &O2);

// computes image of point in cameras image plane
Observation project_point_no_distortion(Camera C, Point3d P)
{
  double p[3];			// rotated into camera frame
  double point[3];		// world location of point
  double aa[3];			// angle axis representation of camera transform
  double tx = C.position[0];	// location of origin in camera frame x
  double ty = C.position[1];    // location of origin in camera frame y
  double tz = C.position[2];	// location of origin in camera frame z
  double fx = C.focal_length_x;	// focal length x
  double fy = C.focal_length_y;	// focal length y
  double cx = C.center_x;	// optical center x
  double cy = C.center_y;	// optical center y

  aa[0] = C.angle_axis[0];
  aa[1] = C.angle_axis[1];
  aa[2] = C.angle_axis[2];
  point[0] = P.x;		
  point[1] = P.y;
  point[2] = P.z;

  /** rotate and translate points into camera frame */
  ceres::AngleAxisRotatePoint(aa, point, p);

  // apply camera translation
  double xp1 = p[0] + tx;
  double yp1 = p[1] + ty;
  double zp1 = p[2] + tz;

  // scale into the image plane by distance away from camera
  double xp = xp1 / zp1;
  double yp = yp1 / zp1;

  // perform projection using focal length and camera center into image plane
  Observation O;
  O.x = fx * xp + cx;
  O.y = fy * yp + cy;
  return (O);
}

struct CameraReprjErrorNoDistortion
{
  CameraReprjErrorNoDistortion(double ob_x, double ob_y, double fx, double fy, double cx, double cy) :
    ox_(ob_x), oy_(ob_y), fx_(fx), fy_(fy), cx_(cx), cy_(cy)
  {
  }

  template<typename T>
  bool operator()(const T* const c_p1, /** extrinsic parameters */
		  const T* point, /** point being projected, yes this is has 3 parameters */
		  T* resid) const
  {
    /** extract the variables from the camera parameters */
    int q = 0; /** extrinsic block of parameters */
    const T& x = c_p1[q++]; /**  angle_axis x for rotation of camera		 */
    const T& y = c_p1[q++]; /**  angle_axis y for rotation of camera */
    const T& z = c_p1[q++]; /**  angle_axis z for rotation of camera */
    const T& tx = c_p1[q++]; /**  translation of camera x */
    const T& ty = c_p1[q++]; /**  translation of camera y */
    const T& tz = c_p1[q++]; /**  translation of camera z */

    /** rotate and translate points into camera frame */
    T aa[3];/** angle axis  */
    T p[3]; /** point rotated */
    aa[0] = x;
    aa[1] = y;
    aa[2] = z;
    ceres::AngleAxisRotatePoint(aa, point, p);

    /** apply camera translation */
    T xp1 = p[0] + tx; /** point rotated and translated */
    T yp1 = p[1] + ty;
    T zp1 = p[2] + tz;

    /** scale into the image plane by distance away from camera */
    T xp = xp1 / zp1;
    T yp = yp1 / zp1;

    /** perform projection using focal length and camera center into image plane */
    resid[0] = T(fx_) * xp + T(cx_) - T(ox_);
    resid[1] = T(fy_) * yp + T(cy_) - T(oy_);

    return true;
  } /** end of operator() */

  /** Factory to hide the construction of the CostFunction object from */
  /** the client code. */
  static ceres::CostFunction* Create(const double o_x, const double o_y, 
				     const double fx, const double fy,
				     const double cx, const double cy)
  {
    return (new ceres::AutoDiffCostFunction<CameraReprjErrorNoDistortion, 2, 6, 3>(new CameraReprjErrorNoDistortion(o_x, o_y, fx, fy, cx, cy)));
  }
  double ox_; /** observed x location of object in image */
  double oy_; /** observed y location of object in image */
  double fx_; /*!< known focal length of camera in x */
  double fy_; /*!< known focal length of camera in y */
  double cx_; /*!< known optical center of camera in x */
  double cy_; /*!< known optical center of camera in y */
};


int main(int argc, char** argv)
{
  vector<Point3d> points;
  vector<Point3d> original_points;
  vector<Camera>  cameras;
  vector<Camera>  original_cameras;
  vector<ObservationDataPoint> original_observations;
  vector<ObservationDataPoint> observations;

  // TODO use a parameter file for these, or make them arguments
  // hard coded constants 
  double camera_pos_noise     = 36.0/12.0; // 36 inches in feet
  double camera_or_noise      = 3.0; // degrees
  double image_noise          = .4; // pixels
  double max_detect_distance  = 999999570.0; // feet at which can't detect object
  int num_test_cases          = 100;

  google::InitGoogleLogging(argv[0]);
  if (argc != 3)
    {
      std::cerr << "usage: BaExCal <3Dpoints_file> <cameras_file>\n";
      return 1;
    }


  ifstream points_input_file(argv[1]);
  if (points_input_file.fail())
    {
      string temp(argv[1]);
      ROS_ERROR_STREAM("ERROR can't open points_input_file:  "<< temp.c_str());
      return (false);
    }

  ifstream cameras_input_file(argv[2]);
  if (cameras_input_file.fail())
    {
      string temp(argv[2]);
      ROS_ERROR_STREAM("ERROR can't open cameras_input_file:  "<< temp.c_str());
      return (false);
    }

  // read in the point and camera data from yaml files
  parse_points(points_input_file,original_points);
  parse_cameras(cameras_input_file,original_cameras);

  // this sets the nominal number of observations for each camera
  // NOTE, it may vary some if the noise is high
  compute_observations(original_cameras,original_points,0.0,max_detect_distance,observations);

  // Setup problem 1 to see if camera extrinsics may be recovered with fixed fiducials
  // create nominal observations of points using camera parameters
  // perturb camera positions and orientations
  // add all observation cost functions to problem
  // solve
  copy_cameras_wo_history(original_cameras,cameras);
  copy_points(original_points,points);
  compute_observations(cameras,points,0.0,max_detect_distance,original_observations);
  compute_observations(cameras,points,image_noise,max_detect_distance,observations);
  compare_observations(original_observations,observations);// shows noise is correct
  perturb_cameras(cameras,camera_pos_noise,camera_or_noise);
  compare_cameras(original_cameras,cameras); // shows how far apart they started

  // Create residuals for each observation in the bundle adjustment problem. The
  // parameters for cameras and points are added automatically.
  ceres::Problem problem1;
  BOOST_FOREACH(ObservationDataPoint obs, observations){
    double x  = obs.image_loc_.x;
    double y  = obs.image_loc_.y;
    double fx = obs.camera_->focal_length_x;
    double fy = obs.camera_->focal_length_y;
    double cx = obs.camera_->center_x;
    double cy = obs.camera_->center_y;

    ceres::CostFunction* cost_function = CameraReprjErrorNoDistortion::Create(x,y,fx,fy,cx,cy);
      
    double *extrinsics = obs.camera_->PB_extrinsics;
    double *points     = obs.point_position_;
    problem1.AddResidualBlock(cost_function, NULL, extrinsics, points);
    problem1.SetParameterBlockConstant(points); // fixed fiducials

  }

  // solve problem
  ceres::Solver::Options options;
  options.linear_solver_type = ceres::DENSE_SCHUR;
  options.minimizer_progress_to_stdout = false;
  options.max_num_iterations = 1000;
  
  // display results
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem1, &summary);
  // std::cout << summary.FullReport() << "\n";

  compare_cameras(original_cameras,cameras); // shows that cameras were recovered

  exit(1);
  copy_cameras_wo_history(original_cameras,cameras);
  copy_points(original_points,points);
  compute_observations(cameras,points,image_noise,max_detect_distance,observations);
  perturb_cameras(cameras,camera_pos_noise,camera_or_noise);

  ceres::Problem problem2;
  BOOST_FOREACH(ObservationDataPoint obs, observations){
    double x  = obs.image_loc_.x;
    double y  = obs.image_loc_.y;
    double fx = obs.camera_->focal_length_x;
    double fy = obs.camera_->focal_length_y;
    double cx = obs.camera_->center_x;
    double cy = obs.camera_->center_y;

    ceres::CostFunction* cost_function = CameraReprjErrorNoDistortion::Create(x,y,fx,fy,cx,cy);
      
    double *extrinsics = obs.camera_->PB_extrinsics;
    double *points     = obs.point_position_;
    problem2.AddResidualBlock(cost_function, NULL, extrinsics, points);
    problem2.SetParameterBlockConstant(points); // fixed fiducials

  }
  ceres::Solve(options, &problem2, &summary);
  std::cout << summary.FullReport() << "\n";


  // clear all pose histories of cameras
  BOOST_FOREACH(Camera &C, original_cameras){
    C.pose_history.clear();
  }

  // compute poses for cameras for a bunch of test cases
  for(int test_case=0; test_case<num_test_cases; test_case++){
    copy_cameras_wo_history(original_cameras, cameras);
    copy_points(original_points,points);
    compute_observations(cameras,points,image_noise,max_detect_distance,observations);
    perturb_cameras(cameras,camera_pos_noise,camera_or_noise);

    ceres::Problem problem;
    BOOST_FOREACH(ObservationDataPoint obs, observations){
      double x  = obs.image_loc_.x;
      double y  = obs.image_loc_.y;
      double fx = obs.camera_->focal_length_x;
      double fy = obs.camera_->focal_length_y;
      double cx = obs.camera_->center_x;
      double cy = obs.camera_->center_y;

      ceres::CostFunction* cost_function = CameraReprjErrorNoDistortion::Create(x,y,fx,fy,cx,cy);
      
      double *extrinsics = obs.camera_->PB_extrinsics;
      double *points     = obs.point_position_;
      problem.AddResidualBlock(cost_function, NULL, extrinsics, points);
      problem.SetParameterBlockConstant(points); // fixed fiducials

    }
    ceres::Solve(options, &problem, &summary);

    add_pose_to_history(cameras, original_cameras);

  }// end of test cases
  compute_historic_pose_statistics(original_cameras);
  
  BOOST_FOREACH(Camera &C, original_cameras){
    printf("%s\t:sigma distance  %lf angular %lf number_observations = %d\n",
	   C.camera_name.c_str(),
	   C.pose_position_sigma*12.0,
	   C.pose_orientation_sigma*180/3.1415,
	   C.num_observations);
  } 
}

// print a quaternion plus position as a homogeneous transform
void print_QTasH(double qx, double qy, double qz, double qw, double tx, double ty, double tz)
{
  double Rs11 = qw * qw + qx * qx - qy * qy - qz * qz;
  double Rs21 = 2.0 * qx * qy + 2.0 * qw * qz;
  double Rs31 = 2.0 * qx * qz - 2.0 * qw * qy;

  double Rs12 = 2.0 * qx * qy - 2.0 * qw * qz;
  double Rs22 = qw * qw - qx * qx + qy * qy - qz * qz;
  double Rs32 = 2.0 * qy * qz + 2.0 * qw * qx;

  double Rs13 = 2.0 * qx * qz + 2.0 * qw * qy;
  double Rs23 = 2.0 * qy * qz - 2.0 * qw * qx;
  double Rs33 = qw * qw - qx * qx - qy * qy + qz * qz;

  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", Rs11, Rs12, Rs13, tx);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", Rs21, Rs22, Rs23, ty);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", Rs31, Rs32, Rs33, tz);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", 0.0, 0.0, 0.0, 1.0);
}

// angle axis to homogeneous transform inverted
void print_AATasH(double x, double y, double z, double tx, double ty, double tz)
{
  double R[9];
  double aa[3];
  aa[0] = x;
  aa[1] = y;
  aa[2] = z;
  ceres::AngleAxisToRotationMatrix(aa, R);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[0], R[3], R[6], tx);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[1], R[4], R[7], ty);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[2], R[5], R[8], tz);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", 0.0, 0.0, 0.0, 1.0);
}

// angle axis to homogeneous transform
void print_AATasHI(double x, double y, double z, double tx, double ty, double tz)
{
  double R[9];
  double aa[3];
  aa[0] = x;
  aa[1] = y;
  aa[2] = z;
  ceres::AngleAxisToRotationMatrix(aa, R);
  double ix = -(tx * R[0] + ty * R[1] + tz * R[2]);
  double iy = -(tx * R[3] + ty * R[4] + tz * R[5]);
  double iz = -(tx * R[6] + ty * R[7] + tz * R[8]);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[0], R[1], R[2], ix);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[3], R[4], R[5], iy);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", R[6], R[7], R[8], iz);
  printf("%6.3lf %6.3lf %6.3lf %6.3lf\n", 0.0, 0.0, 0.0, 1.0);
}

void print_AAasEuler(double x, double y, double z)
{
  double R[9];
  double aa[3];
  aa[0] = x;
  aa[1] = y;
  aa[2] = z;
  ceres::AngleAxisToRotationMatrix(aa, R);
  double rx = atan2(R[7], R[8]);
  double ry = atan2(-R[6], sqrt(R[7] * R[7] + R[8] * R[8]));
  double rz = atan2(R[3], R[0]);
  printf("rpy = %8.4f %8.4f %8.4f\n", rx, ry, rz);
}
void print_camera(Camera C, string words)
{
  printf("%s\n", words.c_str());
  printf("Point in Camera frame to points in World frame Transform:\n");
  print_AATasHI(C.angle_axis[0], C.angle_axis[1], C.angle_axis[2], 
		C.position[0],   C.position[1],   C.position[2]);

  printf("Points in World frame to points in Camera frame transform\n");
  print_AATasH(C.angle_axis[0], C.angle_axis[1], C.angle_axis[2], 
	       C.position[0],   C.position[1],   C.position[2]);

  print_AAasEuler(C.angle_axis[0], C.angle_axis[1], C.angle_axis[2]);
  printf("fx = %8.3lf fy = %8.3lf\n", C.focal_length_x, C.focal_length_y);
  printf("cx = %8.3lf cy = %8.3lf\n", C.center_x, C.center_y);
}


void compute_observations(vector<Camera> &cameras, 
			  vector<Point3d> &points, 
			  double noise,
			  double max_dist,
			  vector<ObservationDataPoint> &observations)
{
  double pnoise = noise/sqrt(1.58085);// magic number found by trial and error
  int n_observations = 0;
  observations.clear();

  // create nominal observations of points using camera parameters
  BOOST_FOREACH(Camera & C, cameras){
    C.num_observations = 0;
    int j=0;
    BOOST_FOREACH(Point3d &P, points){
      j++;
      // find image of point in camera
      double dx = C.position[0] - P.x;
      double dy = C.position[1] - P.y;
      double dz = C.position[2] - P.z;
      double distance = sqrt(dx*dx + dy*dy + dz*dz);
      if(distance < max_dist){
	Observation obs = project_point_no_distortion(C, P);
	obs.x += pnoise*randn(); // add observation noise
	obs.y += pnoise*randn();
	if(obs.x >= 0 && obs.x < C.width && obs.y >= 0 &&obs.y < C.height ){
	  // save observation
	  ObservationDataPoint new_obs(&(C),j,&(P.pb[0]),obs);
	  observations.push_back(new_obs);
	  C.num_observations++;
	  n_observations++;
	} // end if observation within field of view
      } // end if point close enough to camera
    }// end for each fiducial point
   }// end for each camera
}// end compute observations

void perturb_cameras(vector<Camera> &cameras, double position_noise, double degrees_noise)
{
  double pos_noise    = position_noise/sqrt(2.274625);
  double radian_noise = degrees_noise*3.1415/(180.0*sqrt(2.58047));
  BOOST_FOREACH(Camera &C, cameras){
      C.position[0]   += pos_noise*randn();	
      C.position[1]   += pos_noise*randn();
      C.position[2]   += pos_noise*randn();
      C.angle_axis[0] += radian_noise*randn();
      C.angle_axis[1] += radian_noise*randn();
      C.angle_axis[2] += radian_noise*randn();
  }
}

void copy_cameras_wo_history(vector<Camera> &original_cameras, vector<Camera> & cameras)
{
  cameras.clear();
  BOOST_FOREACH(Camera &C, original_cameras){
    Camera newC = C;
    newC.pose_history.clear();
    newC.pose_position_sigma = 0;
    newC.pose_orientation_sigma = 0;
    newC.num_observations = 0;
    cameras.push_back(newC);
  }
}

void copy_points(vector<Point3d> &original_points, vector<Point3d> & points)
{
  points.clear();
  BOOST_FOREACH(Point3d &P, original_points){
    points.push_back(P);
  }
} 

void add_pose_to_history(vector<Camera> &cameras, vector<Camera> & original_cameras)
{
  if(cameras.size() != original_cameras.size())
    ROS_ERROR_STREAM("number of cameras in vectors do not match");

  for(int i=0;i<(int)cameras.size();i++){
    Pose6d pose;
    pose.x  = cameras[i].position[0];
    pose.y  = cameras[i].position[1];
    pose.z  = cameras[i].position[2];
    pose.ax = cameras[i].angle_axis[0];
    pose.ay = cameras[i].angle_axis[1];
    pose.az = cameras[i].angle_axis[2];
    original_cameras[i].pose_history.push_back(pose);
  }
}

void compute_historic_pose_statistics(vector<Camera> & cameras)
{
  // calculate statistics of test case
  double mean_x, mean_y, mean_z, mean_ax, mean_ay, mean_az;
  double sigma_x, sigma_y, sigma_z, sigma_ax, sigma_ay, sigma_az;
  BOOST_FOREACH(Camera &C, cameras){
    mean_x = mean_y = mean_z = mean_ax = mean_ay = mean_az = 0.0;
    BOOST_FOREACH(Pose6d &p, C.pose_history){
      mean_x  += p.x;
      mean_y  += p.y;
      mean_z  += p.z;
      mean_ax += p.ax;
      mean_ay += p.ay;
      mean_az += p.az;
    }
    int num_poses = (int) C.pose_history.size();
    mean_x  = mean_x/num_poses;
    mean_y  = mean_y/num_poses;
    mean_z  = mean_z/num_poses;
    mean_ax = mean_ax/num_poses;
    mean_ay = mean_ay/num_poses;
    mean_az = mean_az/num_poses;

    sigma_x = sigma_y = sigma_z = sigma_ax = sigma_ay, sigma_az = 0.0;
    BOOST_FOREACH(Pose6d &p, C.pose_history){
      sigma_x  += (mean_x - p.x)*(mean_x - p.x);;
      sigma_y  += (mean_y - p.y)*(mean_y - p.y);;
      sigma_z  += (mean_z - p.z)*(mean_z - p.z);;
      sigma_ax += (mean_ax -p.ax)*(mean_ax -p.ax);;
      sigma_ay += (mean_ay -p.ay)*(mean_ay -p.ay);;
      sigma_az += (mean_az -p.az)*(mean_az -p.az);;
    }
    sigma_x  = sqrt(sigma_x/(num_poses + 1.0));
    sigma_y  = sqrt(sigma_y/(num_poses + 1.0));
    sigma_z  = sqrt(sigma_z/(num_poses + 1.0));
    sigma_ax = sqrt(sigma_ax/(num_poses + 1.0));
    sigma_ay = sqrt(sigma_ay/(num_poses + 1.0));
    sigma_az = sqrt(sigma_az/(num_poses + 1.0));

    double dv = sqrt(sigma_x*sigma_x + sigma_y*sigma_y + sigma_z*sigma_z);
    double av = sqrt(sigma_ax*sigma_ax + sigma_ay*sigma_ay + sigma_az*sigma_az);

    C.pose_position_sigma    = dv;
    C.pose_orientation_sigma = av;
  } // end for each camera
}
void parse_points(ifstream &points_input_file,vector<Point3d> &original_points)
{
  // parse points
  try
    {
      YAML::Parser points_parser(points_input_file);
      YAML::Node points_doc;
      points_parser.GetNextDocument(points_doc);

      // read in all points
      const YAML::Node *points_node = points_doc.FindValue("points");
      for (int j = 0; j < points_node->size(); j++){
	const YAML::Node *pnt_node = (*points_node)[j].FindValue("pnt");
	vector<float> temp_pnt;
	(*pnt_node) >> temp_pnt;
	Point3d temp_pnt3d;
	temp_pnt3d.x = temp_pnt[0];
	temp_pnt3d.y = temp_pnt[1];
	temp_pnt3d.z = temp_pnt[2];
	original_points.push_back(temp_pnt3d);
      }
    }
  catch (YAML::ParserException& e){
    ROS_INFO_STREAM("Failed to read points file ");
  }
  ROS_INFO_STREAM("Successfully read in " <<(int) original_points.size() << " points");
}

void parse_cameras(ifstream &cameras_input_file,vector<Camera> & original_cameras)
{
  Camera temp_camera;
  // parse cameras
  try
    {
      YAML::Parser camera_parser(cameras_input_file);
      YAML::Node camera_doc;
      camera_parser.GetNextDocument(camera_doc);

      // read in all static cameras
      if (const YAML::Node *camera_parameters = camera_doc.FindValue("cameras")){
	ROS_INFO_STREAM("Found "<<camera_parameters->size()<<" cameras ");
	for (unsigned int i = 0; i < camera_parameters->size(); i++){
	  (*camera_parameters)[i]["camera_name"]    >> temp_camera.camera_name;
	  // replaced with rotation matrix
	  //        (*camera_parameters)[i]["angle_axis_ax"]  >> temp_camera.angle_axis[0];
	  //        (*camera_parameters)[i]["angle_axis_ay"]  >> temp_camera.angle_axis[1];
	  //        (*camera_parameters)[i]["angle_axis_az"]  >> temp_camera.angle_axis[2];
	  const YAML::Node *rotation_node = (*camera_parameters)[i].FindValue("rotation");
	  if(rotation_node->size() != 9){
	    ROS_ERROR_STREAM("Rotation " << i << " has " << (int)rotation_node->size() << " pts");
	  }
	  // read in the transform from world to camera frame
          // invert it because camera object transforms world points into camera's frame
	  double R[9],tx,ty,tz,angle_axis[3];
	  for(int j=0;j<9;j++){
	    (*rotation_node)[j]  >> R[j];
	  }
	  (*camera_parameters)[i]["position_x"]     >> tx;
	  (*camera_parameters)[i]["position_y"]     >> ty;
	  (*camera_parameters)[i]["position_z"]     >> tz;
	  // NOTE: this Ceres function expects R in column major order, but we read R
	  // in row by row which is effectively the inverse of R
	  // the inverse is the rotation for transforming world points to camera points
	  // but the translation portion is from camera to world
	  double RI[9];
	  RI[0] = R[0];  RI[1] = R[3]; RI[2] = R[6];
	  RI[3] = R[1];  RI[4] = R[4]; RI[5] = R[7];
	  RI[6] = R[2];  RI[7] = R[5]; RI[8] = R[8];
	  ceres::RotationMatrixToAngleAxis(R,angle_axis);
	  temp_camera.position[0] = -(tx * RI[0] + ty * RI[1] + tz * RI[2]);
	  temp_camera.position[1] = -(tx * RI[3] + ty * RI[4] + tz * RI[5]);
	  temp_camera.position[2] = -(tx * RI[6] + ty * RI[7] + tz * RI[8]);

	  temp_camera.angle_axis[0] = angle_axis[0];
	  temp_camera.angle_axis[1] = angle_axis[1];
	  temp_camera.angle_axis[2] = angle_axis[2];
	  (*camera_parameters)[i]["focal_length_x"] >> temp_camera.focal_length_x;
	  (*camera_parameters)[i]["focal_length_y"] >> temp_camera.focal_length_y;
	  (*camera_parameters)[i]["center_x"]       >> temp_camera.center_x;
	  (*camera_parameters)[i]["center_y"]       >> temp_camera.center_y;
	  (*camera_parameters)[i]["width"]          >> temp_camera.width;
	  (*camera_parameters)[i]["height"]         >> temp_camera.height;
	  temp_camera.num_observations = 0.0; // start out with none
	  temp_camera.pose_history.clear(); // start out with no history
	  original_cameras.push_back(temp_camera);
	}	// end of for each camera in file
      } // end if there are any cameras in file
    }
  catch (YAML::ParserException& e){
    ROS_INFO_STREAM("Failed to read in moving cameras from yaml file ");
    ROS_INFO_STREAM("camera name    = " << temp_camera.camera_name.c_str());
    ROS_INFO_STREAM("angle_axis_ax  = " << temp_camera.angle_axis[0]);
    ROS_INFO_STREAM("angle_axis_ay  = " << temp_camera.angle_axis[1]);
    ROS_INFO_STREAM("angle_axis_az  = " << temp_camera.angle_axis[2]);
    ROS_INFO_STREAM("position_x     = " << temp_camera.position[0]);
    ROS_INFO_STREAM("position_y     = " << temp_camera.position[1]);
    ROS_INFO_STREAM("position_z     = " << temp_camera.position[2]);
    ROS_INFO_STREAM("focal_length_x = " << temp_camera.focal_length_x);
    ROS_INFO_STREAM("focal_length_y = " << temp_camera.focal_length_y);
    ROS_INFO_STREAM("center_x       = " << temp_camera.center_x);
    ROS_INFO_STREAM("center_y       = " << temp_camera.center_y);
  }
  ROS_INFO_STREAM("Successfully read in " << (int) original_cameras.size() << " cameras");
}
void compare_cameras(vector<Camera> &C1, vector<Camera> &C2)
{
  if(C1.size()!= C2.size()){
    ROS_ERROR_STREAM("compare_cameras() camera vectors different in length");
  }
  double d_x,d_y,d_z,dist;
  double mean_dist=0;
  double sigma_dist=0;
  double d_ax,d_ay,d_az,angle;
  double mean_angle=0;
  double sigma_angle=0;
  for(int i=0;i<(int)C1.size();i++){
    d_x = C1[i].position[0]-C2[i].position[0];
    d_y = C1[i].position[1]-C2[i].position[1];
    d_z = C1[i].position[2]-C2[i].position[2];
    d_ax = C1[i].angle_axis[0]-C2[i].angle_axis[0];
    d_ay = C1[i].angle_axis[1]-C2[i].angle_axis[1];
    d_az = C1[i].angle_axis[2]-C2[i].angle_axis[2];
    dist = sqrt(d_x*d_x + d_y*d_y + d_z*d_z);
    angle = sqrt(d_ax*d_ax + d_ay*d_ay + d_az*d_az);
    mean_dist += dist;
    mean_angle += angle;
  }
  mean_dist = mean_dist/(int)C1.size();
  mean_angle = mean_angle/(int)C1.size();
  for(int i=0;i<(int)C1.size();i++){
    d_x = C1[i].position[0]-C2[i].position[0];
    d_y = C1[i].position[1]-C2[i].position[1];
    d_z = C1[i].position[2]-C2[i].position[2];
    d_ax = C1[i].angle_axis[0]-C2[i].angle_axis[0];
    d_ay = C1[i].angle_axis[1]-C2[i].angle_axis[1];
    d_az = C1[i].angle_axis[2]-C2[i].angle_axis[2];
    dist = sqrt(d_x*d_x + d_y*d_y + d_z*d_z);
    angle = sqrt(d_ax*d_ax + d_ay*d_ay + d_az*d_az);
    sigma_dist  += (dist-mean_dist)*(dist-mean_dist);
    sigma_angle += (angle-mean_angle)*(angle-mean_angle);
  }
  sigma_dist = sqrt(sigma_dist/C1.size());
  sigma_angle = sqrt(sigma_angle/C1.size());
  ROS_INFO_STREAM("mean distance between camera poses = "<<mean_dist*12 <<" inches " << sigma_dist);
  ROS_INFO_STREAM("mean angle between camera poses = "<<mean_angle*180/3.1415 << " degrees " << sigma_angle*180/3.1415);
}

void compare_observations(vector<ObservationDataPoint> &O1, vector<ObservationDataPoint> &O2)
{
  if(O1.size()!= O2.size()){
    ROS_ERROR_STREAM("compare_observations() vectors different in length");
  }
  double d_x,d_y,dist;
  double mean_dist=0.0;
  for(int i=0;i<(int)O1.size();i++){
    d_x = O1[i].image_loc_.x - O2[i].image_loc_.x;
    d_y = O1[i].image_loc_.y - O2[i].image_loc_.y;
    dist = sqrt(d_x*d_x + d_y*d_y);
    mean_dist += dist;
  }
  mean_dist = mean_dist/(int)O1.size();
  ROS_INFO_STREAM("mean distance between observations = "<<mean_dist <<" pixels");
}
