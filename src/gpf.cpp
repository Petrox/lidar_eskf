#include "lidar_eskf/gpf.h"

GPF::GPF(ros::NodeHandle &nh, boost::shared_ptr<DistMap> map_ptr) : _map_ptr(map_ptr){
    _position_prior.setZero();
    _theta_prior.setZero();
    _rotation_prior.setIdentity();
    _quaternion_prior.setRPY(0.0,0.0,0.0);

    _mean_prior.setZero();
    _mean_posterior.setZero();
    _mean_meas.setZero();

    _cov_prior = 0.01 * Eigen::MatrixXd::Identity(6, 6);
    _cov_posterior.setZero();
    _cov_meas.setZero();

    _cloud_sub = nh.subscribe("cloud", 100, &GPF::cloud_callback, this);
    _odom_sub = nh.subscribe("odom", 100, &GPF::odom_callback, this);
    _meas_pub = nh.advertise<nav_msgs::Odometry>("meas", 100);
    _pset_pub = nh.advertise<visualization_msgs::MarkerArray>("marker", 10);

    _nh_ptr.reset(&nh);
}

void GPF::odom_callback(const nav_msgs::Odometry &msg) {

    _position_prior.setX(msg.pose.pose.position.x);
    _position_prior.setY(msg.pose.pose.position.y);
    _position_prior.setZ(msg.pose.pose.position.z);

    _quaternion_prior.setX(msg.pose.pose.orientation.x);
    _quaternion_prior.setY(msg.pose.pose.orientation.y);
    _quaternion_prior.setZ(msg.pose.pose.orientation.z);
    _quaternion_prior.setW(msg.pose.pose.orientation.w);

    _rotation_prior.setRotation(_quaternion_prior);

    double r, p, y;
    _rotation_prior.getRPY(r, p, y);

    _theta_prior.setX(r);
    _theta_prior.setX(p);
    _theta_prior.setX(y);

    _mean_prior[0] = _position_prior.x();
    _mean_prior[1] = _position_prior.y();
    _mean_prior[2] = _position_prior.z();

    _mean_prior[3] = _theta_prior.x();
    _mean_prior[3] = _theta_prior.x();
    _mean_prior[3] = _theta_prior.x();

    for(int i=0; i<6; i++) {
        for(int j=0; j<6; j++) {
            _cov_prior(i,j) = msg.pose.covariance[i*6+j];
        }
    }
}

void GPF::cloud_callback(const sensor_msgs::PointCloud2 &msg) {
    _laser_time = msg.header.stamp;

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ptr = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::fromROSMsg(msg, *cloud_ptr);
    _cloud_ptr = cloud_ptr;
    downsample();

    Particles pset(_nh_ptr, _mean_prior, _cov_prior, _cloud_ptr, _map_ptr);
    pset.init_set();
    pset.weight_set();

    publish_pset(pset);

//    pset.propagate(_mean_posterior, _cov_posterior);

//    recover_meas();
//    publish_meas();
}

void GPF::downsample() {
    pcl::PointCloud<pcl::PointXYZ>::Ptr unif_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr xlim_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr ylim_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr zlim_cloud = pcl::PointCloud<pcl::PointXYZ>::Ptr (new pcl::PointCloud<pcl::PointXYZ>);

    // down sampling
    pcl::PointCloud<int> sampled_indices;
    pcl::UniformSampling<pcl::PointXYZ> uniform_sampling;
    uniform_sampling.setInputCloud(_cloud_ptr);
    uniform_sampling.setRadiusSearch(0.1);
    uniform_sampling.compute(sampled_indices);
    pcl::copyPointCloud (*_cloud_ptr, sampled_indices.points, *unif_cloud);

    ROS_INFO("GPF: downsample: cloud size = %d, unif size = %d", (int) _cloud_ptr->size(), (int)unif_cloud->size());

    // truncating in range
    double rangeLim = 15.0;
    pcl::PassThrough<pcl::PointXYZ> pass;
    pass.setInputCloud(unif_cloud);
    pass.setFilterFieldName("x");
    pass.setFilterLimits (-rangeLim, rangeLim);
    pass.filter(*xlim_cloud);

    pass.setInputCloud(xlim_cloud);
    pass.setFilterFieldName("y");
    pass.setFilterLimits (-rangeLim, rangeLim);
    pass.filter(*ylim_cloud);

    pass.setInputCloud(ylim_cloud);
    pass.setFilterFieldName("z");
    pass.setFilterLimits (-rangeLim, rangeLim);
    pass.filter(*zlim_cloud);

    _cloud_ptr.reset();
    _cloud_ptr = zlim_cloud;

    // truncate in a bounding range
    pcl::ConditionOr<pcl::PointXYZ>::Ptr rangeCond (new pcl::ConditionOr<pcl::PointXYZ> ());
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("z", pcl::ComparisonOps::LT, -0.5)));
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("z", pcl::ComparisonOps::GT, 0.5)));
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("y", pcl::ComparisonOps::LT, -0.5)));
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("y", pcl::ComparisonOps::GT, 0.5)));
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("x", pcl::ComparisonOps::LT, -0.5)));
    rangeCond->addComparison (pcl::FieldComparison<pcl::PointXYZ>::ConstPtr (new
          pcl::FieldComparison<pcl::PointXYZ> ("x", pcl::ComparisonOps::GT, 0.5)));

    pcl::ConditionalRemoval<pcl::PointXYZ> condRem;
    condRem.setCondition((rangeCond));
    condRem.setInputCloud(_cloud_ptr);
    condRem.setKeepOrganized(true);
    condRem.filter (*_cloud_ptr);
}

void GPF::recover_meas() {
    Eigen::Matrix<double, 6, 6> C;
    Eigen::Matrix<double, 6, 6> K;
    Eigen::Matrix<double, 6, 6> H = Eigen::MatrixXd::Identity(6, 6);

    C = (_cov_posterior.inverse() - _cov_prior.inverse()).inverse();
    check_posdef(C);

    K = _cov_prior.inverse() * H.transpose() * (H * _cov_prior * H.transpose() + C).inverse();
    _mean_meas = K.inverse() * (_mean_posterior - _mean_prior) + _mean_prior;
    _cov_meas = C;
}

void GPF::check_posdef(Eigen::Matrix<double, STATE_SIZE, STATE_SIZE> &R) {
    Eigen::Matrix<double, STATE_SIZE, STATE_SIZE> rot;
    Eigen::Matrix<double, STATE_SIZE, 1> scl;

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, STATE_SIZE, STATE_SIZE> >
       eigenSolver(R);
    rot = eigenSolver.eigenvectors();
    scl = eigenSolver.eigenvalues();

    Eigen::Matrix<double, STATE_SIZE, STATE_SIZE> E;
    E.setZero();
    for (int ii=0; ii<STATE_SIZE; ++ii) {
        if(scl(ii,0)>0.0) {
            E(ii,ii) = scl(ii,0);
        }
        else {
            E(ii,ii) = 100.0;
        }
    }
    R = rot * E * rot.inverse();
}

void GPF::publish_meas() {
    nav_msgs::Odometry msg;

    msg.header.stamp = _laser_time;
    msg.header.frame_id = "world";

    msg.pose.pose.position.x = _mean_meas[0];
    msg.pose.pose.position.y = _mean_meas[1];
    msg.pose.pose.position.z = _mean_meas[2];

    tf::Quaternion q;
    q.setRPY(_mean_meas[3], _mean_meas[4], _mean_meas[5]);
    msg.pose.pose.orientation.x = q.x();
    msg.pose.pose.orientation.y = q.y();
    msg.pose.pose.orientation.z = q.z();
    msg.pose.pose.orientation.w = q.w();

    for(int i=0; i<6; i++) {
        for(int j=0; j<6; j++) {
            msg.pose.covariance[6*i+j] = _cov_meas(i, j);
        }
    }

    _meas_pub.publish(msg);
}

void GPF::publish_pset(Particles p) {

    std::vector<std::vector<double> > c = compute_color(p);
    std::vector<Particle> pset = p.get_pset();

    visualization_msgs::MarkerArray msg;

    for(int i=0; i<SET_SIZE; i++) {

        tf::Vector3 position;
        tf::Quaternion quaternion;

        position.setValue(pset[i].state[0], pset[i].state[1], pset[i].state[2]);
        quaternion.setRPY(pset[i].state[3], pset[i].state[4], pset[i].state[5]);

        visualization_msgs::Marker m;
        m.header.frame_id = "world";
        m.header.stamp = _laser_time;
        m.ns = "particle_set";
        m.id = i;
        m.type = visualization_msgs::Marker::ARROW;
        m.action = visualization_msgs::Marker::ADD;
        m.pose.position.x = position.x();
        m.pose.position.y = position.y();
        m.pose.position.z = position.z();
        m.pose.orientation.x = quaternion.x();
        m.pose.orientation.y = quaternion.y();
        m.pose.orientation.z = quaternion.z();
        m.pose.orientation.w = quaternion.w();
        m.scale.x = 0.1;
        m.scale.y = 0.01;
        m.scale.z = 0.01;
        m.color.a = 1.0; // Don't forget to set the alpha!
        m.color.r = c[i][0];
        m.color.g = c[i][1];
        m.color.b = c[i][2];

        msg.markers.push_back(m);
    }
   _pset_pub.publish(msg);
}

std::vector< std::vector<double> > GPF::compute_color(Particles pSet) {

    std::vector<Particle> particle = pSet.get_pset();
    std::vector< std::vector<double> > color;

    // find maximum and minimum weight
    double minWeight = INFINITY;
    double maxWeight = -INFINITY;
    for(int i=0; i<particle.size(); i++) {
        Particle p = particle[i];
        if(p.weight > maxWeight) maxWeight = p.weight;
        if(p.weight < minWeight) minWeight = p.weight;
    }
    double midWeight = (maxWeight + minWeight)/2.0;

    // compute color for each particle
    for(int i=0; i<particle.size(); i++) {

        std::vector<double> cRGB;
        Particle p = particle[i];
        if(minWeight <= p.weight && p.weight < midWeight) {
            cRGB.push_back((p.weight - minWeight)/(midWeight - minWeight));
            cRGB.push_back(1.0);
            cRGB.push_back(0.0);
        }
        else if(midWeight <= p.weight && p.weight <= maxWeight) {
            cRGB.push_back(1.0);
            cRGB.push_back(1.0 - (p.weight - midWeight)/(maxWeight - midWeight));
            cRGB.push_back(0.0);
        }
        else {
            cRGB.push_back(0.0);
            cRGB.push_back(0.0);
            cRGB.push_back(1.0);

        }
        color.push_back(cRGB);
    }
    return color;
}