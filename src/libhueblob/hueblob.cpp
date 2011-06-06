#include <boost/foreach.hpp>
#include <boost/scope_exit.hpp>

#include <ros/ros.h>

#include <image_transport/image_transport.h>
#include <sensor_msgs/Image.h>
#include <hueblob/Blob.h>
#include <hueblob/AddObject.h>

#include "libhueblob/hueblob.hh"

#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <highgui.h>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <boost/format.hpp>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/features/feature.h>
#include "pcl/filters/statistical_outlier_removal.h"
//#include <pcl_visualization/cloud_viewer.h>
#include <Eigen/Dense>
void nullDeleter(void*) {}
void nullDeleterConst(const void*) {}

HueBlob::HueBlob()
  : nh_("hueblob"),
    it_(nh_),
    stereo_topic_prefix_ (),
    threshold_(),
    bridgeLeft_(),
    bridgeDisparity_(),
    left_sub_(),
    right_sub_(),
    disparity_sub_(),
    sync_(3),
    blobs_pub_(nh_.advertise<hueblob::Blobs>("blobs", 5)),
    cloud_pub_(nh_.advertise<pcl::PointCloud<pcl::PointXYZ> > ("points2", 1)),
    AddObject_srv_(nh_.advertiseService
		   ("add_object", &HueBlob::AddObjectCallback, this)),
    ListObject_srv_(nh_.advertiseService
		    ("list_object", &HueBlob::ListObjectCallback, this)),
    RmObject_srv_(nh_.advertiseService
		  ("rm_object", &HueBlob::RmObjectCallback, this)),
    TrackObject_srv_(nh_.advertiseService
		   ("track_object", &HueBlob::TrackObjectCallback, this)),
    left_objects_(),
    right_objects_(),
    check_synced_timer_(),
    left_received_(),
    right_received_(),
    disp_received_(),
    all_received_(),
    leftImage_(),
    rightImage_(),
    leftCamera_(),
    disparity_()
{
  // Parameter initialization.
  ros::param::param<std::string>("~stereo", stereo_topic_prefix_, "");
  ros::param::param<std::string>("~algo", algo_, "camshift");
  ros::param::param<std::string>("~stereo", stereo_topic_prefix_, "");
  ros::param::param<double>("threshold", threshold_, 75.);

  tracked_left_pub_ = it_.advertise("/hueblob/tracked/left/image_rec_color", 1);
  tracked_right_pub_ = it_.advertise("/hueblob/tracked/right/image_rec_color", 1);


  // Initialize the node subscribers, publishers and filters.
  setupInfrastructure(stereo_topic_prefix_);


}

HueBlob::~HueBlob()
{
  ROS_DEBUG("Destructing the node.");
}

void
HueBlob::spin()
{
  typedef std::pair<const std::string&, const Object&> iterator_t;

  ros::Rate loop_rate(10);

  ROS_DEBUG("Entering the node main loop.");
  cv::Mat img;
  while (ros::ok())
    {
      hueblob::Blobs blobs;

      if (leftImage_)
        img = bridgeLeft_.imgMsgToCv(leftImage_, "bgr8");


      //FIXME: currently iterate on all blobs.
      // Should we prune untrackable blobs?
      // Should we provide a way to disable tracking
      //  for some objects?
      BOOST_FOREACH(iterator_t it, left_objects_)
	{
	  hueblob::Blob blob = trackBlob(it.first);
	  blobs.blobs.push_back(blob);
	}

      for (  std::vector<hueblob::Blob>::iterator iter= blobs.blobs.begin();
             iter != blobs.blobs.end(); iter++ )
        {
          int x =  (*iter).boundingbox_2d[0];
          int y =  (*iter).boundingbox_2d[1];
          int width =  (*iter).boundingbox_2d[2];
          int height =  (*iter).boundingbox_2d[3];
          cv::Point p1(x, y);
          cv::Point p2(x + width, y + height);
          cv::Point pc(x, y + std::max(16, height+8));
          const cv::Scalar color = CV_RGB(255,0,0);
          // ROS_DEBUG_STREAM("Drawing rect " << x << " " << " " << y
          //                  << " " << width << " " << height);
          cv::rectangle(img, p1, p2, color, 1);
          stringstream ss (stringstream::in | stringstream::out);
          boost::format fmter("[%3.3f %3.3f %3.3f %1.2f]");
          (fmter % (*iter).cloud_centroid.transform.translation.x
           %  (*iter).cloud_centroid.transform.translation.y
           %  (*iter).cloud_centroid.transform.translation.z
           %  (*iter).depth_density
           );
          cv::putText(img, fmter.str(), p1, CV_FONT_HERSHEY_SIMPLEX, 0.5, color);

          boost::format fmter2("[%3.3f %3.3f %3.3f]");
          (fmter2 % (*iter).position.transform.translation.x
           %  (*iter).position.transform.translation.y
           %  (*iter).position.transform.translation.z
           );
          cv::putText(img, fmter2.str(), pc, CV_FONT_HERSHEY_SIMPLEX, 0.5, color);
        }

      if (leftImage_){
        cv_bridge::CvImage brd_im;
        brd_im.image = img;
        brd_im.header = leftImage_->header;
        brd_im.encoding = sensor_msgs::image_encodings::TYPE_8UC3;
        tracked_left_pub_.publish(brd_im.toImageMsg());
      }

      blobs_pub_.publish(blobs);
      ros::spinOnce();
      loop_rate.sleep();
    }
}

namespace
{
  void increment(int* value)
  {
    ++(*value);
  }
} // end of anonymous namespace.

void
HueBlob::setupInfrastructure(const std::string& stereo_prefix)
{
  stereo_topic_prefix_ = nh_.resolveName(stereo_prefix);

  const std::string left_topic =
    ros::names::clean(stereo_topic_prefix_ + "/left/image_rect_color");
  const std::string left_camera_topic =
    ros::names::clean(stereo_topic_prefix_ + "/left/camera_info");
  const std::string right_topic =
    ros::names::clean(stereo_topic_prefix_ + "/right/image_rect_color");
  const std::string right_camera_topic =
    ros::names::clean(stereo_topic_prefix_ + "/right/camera_info");
  const std::string disparity_topic =
    ros::names::clean(stereo_topic_prefix_ + "/disparity");

  left_sub_.subscribe(it_, left_topic, 3);
  leftCamera_sub_.subscribe(nh_, left_camera_topic, 3);
  right_sub_.subscribe(it_, right_topic, 3);
  rightCamera_sub_.subscribe(nh_, right_camera_topic, 3);
  disparity_sub_.subscribe(nh_, disparity_topic, 3);

  //FIXME: is it needed to be reentrant?
  //sync_.disconnectAll();
  sync_.connectInput(left_sub_, leftCamera_sub_,
		     right_sub_, rightCamera_sub_,
		     disparity_sub_);
  sync_.registerCallback(boost::bind(&HueBlob::imageCallback,
				     this, _1, _2, _3, _4, _5));

  // Complain every 30s if the topics appear unsynchronized
  left_sub_.registerCallback(boost::bind(increment, &left_received_));
  right_sub_.registerCallback(boost::bind(increment, &right_received_));
  disparity_sub_.registerCallback(boost::bind(increment, &disp_received_));
  sync_.registerCallback(boost::bind(increment, &all_received_));
  check_synced_timer_ =
    nh_.createWallTimer(ros::WallDuration(30.0),
			boost::bind(&HueBlob::checkInputsSynchronized, this));

  ROS_INFO("Subscribing to:\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s\n"
	   "\t* %s",
	   left_topic.c_str(), left_camera_topic.c_str(),
	   right_topic.c_str(), right_camera_topic.c_str(),
	   disparity_topic.c_str());
}

void
HueBlob::imageCallback(const sensor_msgs::ImageConstPtr& left,
		       const sensor_msgs::CameraInfoConstPtr& left_camera,
		       const sensor_msgs::ImageConstPtr& right,
		       const sensor_msgs::CameraInfoConstPtr& right_camera,
		       const stereo_msgs::DisparityImageConstPtr& disparity)
{
  leftImage_ = left;
  rightImage_ = right;
  leftCamera_ = left_camera;
  disparity_ = disparity;
}

bool
HueBlob::AddObjectCallback(hueblob::AddObject::Request& request,
			   hueblob::AddObject::Response& response)
{
  response.status = 0;

  // Convert ROS image to OpenCV.
  IplImage* model_ = 0;
  sensor_msgs::CvBridge bridge;
  try
    {
      boost::shared_ptr<sensor_msgs::Image> image_ptr
	(&request.image, nullDeleter);
      model_ = bridge.imgMsgToCv(image_ptr,"bgr8");
    }
  catch(const sensor_msgs::CvBridgeException& error)
    {
      ROS_ERROR("failed to convert image");
      return false;
    }
  cv::Mat model(model_, false);


  // Get reference on the object.
  Object& left_object = left_objects_[request.name];
  Object& right_object = right_objects_[request.name];
  if (algo_ == "naive")
    {
      left_object.algo = NAIVE;
      right_object.algo = NAIVE;
    }
  else if (algo_ == "camshift")
    {
      left_object.algo = CAMSHIFT;
      right_object.algo = CAMSHIFT;
    }


  // Emit a warning if the object already exists.
  if (left_object.anchor_x
      || left_object.anchor_y
      || left_object.anchor_z)
    ROS_WARN("Overwriting the object %s", request.name.c_str());

  // Initialize the object.
  left_object.anchor_x = request.anchor.x;
  left_object.anchor_y = request.anchor.y;
  left_object.anchor_z = request.anchor.z;
  // Add the view to the object.
  left_object.addView(model);

  right_object.anchor_x = request.anchor.x;
  right_object.anchor_y = request.anchor.y;
  right_object.anchor_z = request.anchor.z;
  // Add the view to the object.
  right_object.addView(model);

  return true;
}

bool
HueBlob::ListObjectCallback(hueblob::ListObject::Request& request,
			    hueblob::ListObject::Response& response)
{
  typedef std::pair<const std::string&, const Object&> iterator_t;
  BOOST_FOREACH(iterator_t it, left_objects_)
    response.objects.push_back(it.first);
  return true;
}

bool
HueBlob::RmObjectCallback(hueblob::RmObject::Request& request,
			  hueblob::RmObject::Response& response)
{
  left_objects_.erase(request.name);
  right_objects_.erase(request.name);
  return true;
}

bool
HueBlob::TrackObjectCallback(hueblob::TrackObject::Request& request,
			     hueblob::TrackObject::Response& response)
{
  return true;
}

namespace
{



  void get3dCloud(const cv::Mat& image,
                  const cv::Mat& disparity,
                  cv::Rect& rect,
                  cv::Rect& right_rect,
                  const double f,
                  const double T,
                  const double Z_min,
                  const double Z_max,
                  const double u0,
                  const double v0,
                  const double px,
                  const double py,
                  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud,
                  cv::Point3f& center_est
                  )
  {
    cv::Point2f right_center(right_rect.x
                             + right_rect.width*0.5,
                             right_rect.y
                             + right_rect.height*0.5);
    cv::Point2f left_center(rect.x
                            + rect.width*0.5,
                            rect.y
                            + rect.height*0.5);

    // Make sure the rectangle is valid.
    // check if the size of two rect are not too different
    if (std::fabs(right_center.y - left_center.y) > 3 ||
        1.0*(rect.width/right_rect.width) > 1.5 ||
        1.0*(rect.width/right_rect.width) < 0.5
        )
      {
        center_est.x = 0;
        center_est.y = 0;
        center_est.z = 0;
      }
    else
      {
        //ROS_DEBUG_STREAM(right_center << " " << left_center << " ");
        rect.x = std::max(0, rect.x);
        rect.y = std::max(0, rect.y);
        rect.width = std::min(disparity.rows, rect.width);
        rect.height = std::min(disparity.cols, rect.height);
        float d = left_center.x - right_center.x;
        double x = left_center.x;
        double y = left_center.y;
        center_est.x = (x - u0) / px;
        center_est.y = (y - v0) / py;
        center_est.z = (1.0*f * T / d);
        // ROS_DEBUG_STREAM(left_center << " "
        //                  << right_center << " "
        //                  << center_est);
      }
    for (int y = rect.y; y < rect.y + rect.height; ++y)
      for (int x = rect.x; x < rect.x + rect.width; ++x)
	{
	  unsigned char d = disparity.at<unsigned char>(y, x);
	  //FIXME: check transformation.
	  double X = (x - u0) / px;
	  double Y = (y - v0) / py;
	  double Z = (d > 0) ? (1.0*f * T / d) : 0.;

	  // If the point is not part of the horopter, ignore it.
	  if (Z < Z_min || Z > Z_max)
	    continue;
          pcl::PointXYZ point(X,Y,Z);
          pcl_cloud->points.push_back(point);
        }
  }

  //FIXME: this is not as good as the original hueblob...
  void get3dBox(const cv::Mat& image,
		const cv::Mat& disparity,
		cv::Rect& rect,
		const double f,
		const double T,
		const double Z_min,
		const double Z_max,
		const double u0,
		const double v0,
		const double px,
		const double py,
		cv::Point3d& min,
		cv::Point3d& max,
		cv::Point3d& center)
  {
    // Make sure the rectangle is valid.

    rect.x = std::max(0, rect.x);
    rect.y = std::max(0, rect.y);
    rect.width = std::min(disparity.rows, rect.width);
    rect.height = std::min(disparity.cols, rect.height);
    for (int y = rect.y; y < rect.y + rect.height; ++y)
      for (int x = rect.x; x < rect.x + rect.width; ++x)
	{
	  unsigned char d = disparity.at<unsigned char>(y, x);
	  //FIXME: check transformation.
	  double X = (x - u0) / px;
	  double Y = (y - v0) / py;
	  double Z = (d > 0) ? (1.0*f * T / d) : 0.;
	  cv::Point3d p(X, Y, Z);

	  // If the point is not part of the horopter, ignore it.
	  if (Z < Z_min || Z > Z_max)
            // ROS_DEBUG_STREAM_THROTTLE(0.2,"x=" << x
            //                           << " y=" << y
            //                           << " d=" << static_cast<int>(d)
            //                           << "\nignoring: Z=" << Z
            //                           << " Z_min=" << Z_min
            //                           << " Z_max=" << Z_max);
	    continue;

	  // Update extrema.
	  if (p.x < min.x)
	    min.x = p.x;
	  if (p.y < min.y)
	    min.y = p.y;
	  if (p.z < min.z)
	    min.z = p.z;

	  if (p.x > max.x)
	    max.x = p.x;
	  if (p.y > max.y)
	    max.y = p.y;
	  if (p.z > max.z)
	    max.z = p.z;
          //ROS_DEBUG_STREAM(p.x << " " << p.y << " " << p.z);
	}
    center.x = std::fabs(max.x - min.x) / 2.;
    center.y = std::fabs(max.y - min.y) / 2.;
    center.z = std::fabs(max.z - min.z) / 2.;
  }
} // end of anonymous namespace.

hueblob::Blob
HueBlob::trackBlob(const std::string& name)
{
  hueblob::Blob blob;

  // Fill blob header.
  blob.name = name;
  blob.position.header = leftImage_->header;
  blob.position.child_frame_id = "/hueblob_" + name;
  blob.boundingbox_2d.resize(4);
  for (unsigned i = 0; i < 4; ++i)
    blob.boundingbox_2d[i] = 0.;

  // Image acquisition.
  if (!leftImage_ || !disparity_)
    return blob;

  // Realize 2d tracking in the image.
  Object& robject = right_objects_[name];
  // get box for right image
  cv::Mat right_image(bridgeLeft_.imgMsgToCv(rightImage_, "bgr8"), false);
  boost::optional<cv::RotatedRect> right_rrect = robject.track(right_image);

  Object& object = left_objects_[name];
  cv::Mat image(bridgeLeft_.imgMsgToCv(leftImage_, "bgr8"), false);
  boost::optional<cv::RotatedRect> rrect = object.track(image);
  if (!rrect || !right_rrect)
    {
      ROS_WARN_THROTTLE(20, "failed to track object");
      return blob;
    }

  cv::Rect rect = rrect->boundingRect();
  cv::Rect right_rect = right_rrect->boundingRect();

  blob.boundingbox_2d[0] = rect.x;
  blob.boundingbox_2d[1] = rect.y;
  blob.boundingbox_2d[2] = rect.width;
  blob.boundingbox_2d[3] = rect.height;

  if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0)
    {
      ROS_WARN_THROTTLE
        (20, "failed to track object (invalid tracking window)");
      return blob;
    }

  // Convert disparity to OpenCV image.
  boost::shared_ptr<const sensor_msgs::Image> imagePtr
    (&disparity_->image, nullDeleterConst);
  cv::Mat disparity(bridgeDisparity_.imgMsgToCv(imagePtr, "mono8"), false);

  // Compute 3d box.
  const double f = disparity_->f;
  const double T = disparity_->T;
  const double Z_min = f * T / disparity_->max_disparity;
  const double Z_max = f * T / disparity_->min_disparity;
  const double u0 = leftCamera_->K[0 * 3 + 0];
  const double v0 = leftCamera_->K[1 * 3 + 1];
  const double px = leftCamera_->K[0 * 3 + 2];
  const double py = leftCamera_->K[1 * 3 + 2];
  cv::Point3d min;
  cv::Point3d max;
  cv::Point3d center;
  // static pcl_visualization::CloudViewer viewer("Simple Cloud Viewer");
  pcl::PointCloud<pcl::PointXYZ>::Ptr pcl_cloud(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered (new pcl::PointCloud<pcl::PointXYZ>);
  cv::Point3f center_est;
  get3dCloud(image, disparity, rect, right_rect, f, T,
             Z_min, Z_max, u0, v0, px, py,
             pcl_cloud, center_est);
  // std::cerr << "Cloud before filtering: " << std::endl;
  // std::cerr << *pcl_cloud << std::endl;
  float depth_density = 1.*pcl_cloud->points.size()/(rect.width*rect.height);

  Eigen::Vector4f centroid (0., 0., 0., 0.);
  Eigen::Vector4f min3d (0., 0., 0., 0.);
  Eigen::Vector4f max3d (0., 0., 0., 0.);
  if (pcl_cloud->points.size() >0)
    {
      cloud_pub_.publish(pcl_cloud);
      pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
      sor.setInputCloud (pcl_cloud);
      sor.setMeanK (50);
      sor.setStddevMulThresh (1.0);
      sor.filter (*cloud_filtered);
      cloud_filtered->header.frame_id = "test";
      cloud_filtered->header.stamp = leftImage_->header.stamp;
      pcl::compute3DCentroid(*cloud_filtered, centroid);
      pcl::getMinMax3D(*cloud_filtered, min3d, max3d);
      // ROS_DEBUG_STREAM(rect.width << " " << rect.height
      //                  << " " << depth_density
      //                  << "\n" <<  min3d << "\n" << max3d);
      cloud_pub_.publish(cloud_filtered);

      //      viewer.showCloud(*cloud_filtered);
    }
  // std::cerr << "Cloud after filtering: " << std::endl;
  // std::cerr << *cloud_filtered << std::endl;

  center.x = centroid[0];
  center.y = centroid[1];
  center.z = centroid[2];
  center.x += object.anchor_x;
  center.y += object.anchor_y;
  center.z += object.anchor_z;

  // Fill blob.
  blob.cloud_centroid.transform.translation.x = center.x;
  blob.cloud_centroid.transform.translation.y = center.y;
  blob.cloud_centroid.transform.translation.z = center.z;
  blob.cloud_centroid.transform.rotation.x = 0.;
  blob.cloud_centroid.transform.rotation.y = 0.;
  blob.cloud_centroid.transform.rotation.z = 0.;
  blob.cloud_centroid.transform.rotation.w = 0.;

  blob.position.transform.translation.x = center_est.x;
  blob.position.transform.translation.y = center_est.y;
  blob.position.transform.translation.z = center_est.z;
  blob.position.transform.rotation.x = 0.;
  blob.position.transform.rotation.y = 0.;
  blob.position.transform.rotation.z = 0.;
  blob.position.transform.rotation.w = 0.;
  blob.depth_density = depth_density;
  return blob;
}

void
HueBlob::checkInputsSynchronized()
{
  int threshold = 3 * all_received_;
  if (left_received_ > threshold
      || right_received_ > threshold || disp_received_ > threshold)
    {
      ROS_WARN
	("[hueblob] Low number of synchronized left/right/disparity triplets"
	 " received.\n"
	 "Left images received: %d\n"
	 "Right images received: %d\n"
	 "Disparity images received: %d\n"
	 "Synchronized triplets: %d\n"
	 "Possible issues:\n"
	 "\t* stereo_image_proc is not running.\n"
	 "\t* The cameras are not synchronized.\n"
	 "\t* The network is too slow. One or more images are dropped from each"
	 "triplet.",
	 left_received_, right_received_, disp_received_, all_received_);
    }
}
