// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2023 Intel Corporation. All Rights Reserved.

#include "rs-dds-device-proxy.h"
#include "rs-dds-color-sensor-proxy.h"
#include "rs-dds-depth-sensor-proxy.h"

#include <realdds/dds-device.h>
#include <realdds/dds-stream.h>
#include <realdds/dds-trinsics.h>

#include <realdds/topics/device-info-msg.h>

#include <src/stream.h>
#include <src/environment.h>

#include <rsutils/json.h>


namespace librealsense {


// Constants for Json lookups
static const std::string stream_name_key( "stream-name", 11 );


static rs2_stream to_rs2_stream_type( std::string const & type_string )
{
    static const std::map< std::string, rs2_stream > type_to_rs2 = {
        { "depth", RS2_STREAM_DEPTH },
        { "color", RS2_STREAM_COLOR },
        { "ir", RS2_STREAM_INFRARED },
        { "fisheye", RS2_STREAM_FISHEYE },
        { "gyro", RS2_STREAM_GYRO },
        { "accel", RS2_STREAM_ACCEL },
        { "gpio", RS2_STREAM_GPIO },
        { "pose", RS2_STREAM_POSE },
        { "confidence", RS2_STREAM_CONFIDENCE },
    };
    auto it = type_to_rs2.find( type_string );
    if( it == type_to_rs2.end() )
    {
        LOG_ERROR( "Unknown stream type '" << type_string << "'" );
        return RS2_STREAM_ANY;
    }
    return it->second;
}


static rs2_video_stream to_rs2_video_stream( rs2_stream const stream_type,
                                             sid_index const & sidx,
                                             std::shared_ptr< realdds::dds_video_stream_profile > const & profile,
                                             const std::set< realdds::video_intrinsics > & intrinsics )
{
    rs2_video_stream prof = {};
    prof.type = stream_type;
    prof.index = sidx.index;
    prof.uid = sidx.sid;
    prof.width = profile->width();
    prof.height = profile->height();
    prof.fps = profile->frequency();
    prof.fmt = static_cast< rs2_format >( profile->format().to_rs2() );

    // Handle intrinsics
    auto intr = std::find_if( intrinsics.begin(),
                              intrinsics.end(),
                              [profile]( const realdds::video_intrinsics & intr )
                              { return profile->width() == intr.width && profile->height() == intr.height; } );
    if( intr != intrinsics.end() )  // Some profiles don't have intrinsics
    {
        prof.intrinsics.width = intr->width;
        prof.intrinsics.height = intr->height;
        prof.intrinsics.ppx = intr->principal_point_x;
        prof.intrinsics.ppy = intr->principal_point_y;
        prof.intrinsics.fx = intr->focal_lenght_x;
        prof.intrinsics.fy = intr->focal_lenght_y;
        prof.intrinsics.model = static_cast< rs2_distortion >( intr->distortion_model );
        memcpy( prof.intrinsics.coeffs, intr->distortion_coeffs.data(), sizeof( prof.intrinsics.coeffs ) );
    }

    return prof;
}


static rs2_motion_stream to_rs2_motion_stream( rs2_stream const stream_type,
                                               sid_index const & sidx,
                                               std::shared_ptr< realdds::dds_motion_stream_profile > const & profile,
                                               const realdds::motion_intrinsics & intrinsics )
{
    rs2_motion_stream prof;
    prof.type = stream_type;
    prof.index = sidx.index;
    prof.uid = sidx.sid;
    prof.fps = profile->frequency();
    prof.fmt = static_cast< rs2_format >( profile->format().to_rs2() );

    memcpy( prof.intrinsics.data, intrinsics.data.data(), sizeof( prof.intrinsics.data ) );
    memcpy( prof.intrinsics.noise_variances,
            intrinsics.noise_variances.data(),
            sizeof( prof.intrinsics.noise_variances ) );
    memcpy( prof.intrinsics.bias_variances,
            intrinsics.bias_variances.data(),
            sizeof( prof.intrinsics.bias_variances ) );

    return prof;
}


static rs2_extrinsics to_rs2_extrinsics( const std::shared_ptr< realdds::extrinsics > & dds_extrinsics )
{
    rs2_extrinsics rs2_extr;

    memcpy( rs2_extr.rotation, dds_extrinsics->rotation.data(), sizeof( rs2_extr.rotation ) );
    memcpy( rs2_extr.translation, dds_extrinsics->translation.data(), sizeof( rs2_extr.translation ) );

    return rs2_extr;
}


dds_device_proxy::dds_device_proxy( std::shared_ptr< context > ctx, std::shared_ptr< realdds::dds_device > const & dev )
    : software_device( ctx )
    , _dds_dev( dev )
{
    LOG_DEBUG( "=====> dds-device-proxy " << this << " created on top of dds-device " << _dds_dev.get() );
    register_info( RS2_CAMERA_INFO_NAME, dev->device_info().name );
    register_info( RS2_CAMERA_INFO_SERIAL_NUMBER, dev->device_info().serial );
    register_info( RS2_CAMERA_INFO_PRODUCT_LINE, dev->device_info().product_line );
    register_info( RS2_CAMERA_INFO_PRODUCT_ID, dev->device_info().product_id );
    register_info( RS2_CAMERA_INFO_PHYSICAL_PORT, dev->device_info().topic_root );
    register_info( RS2_CAMERA_INFO_CAMERA_LOCKED, dev->device_info().locked ? "YES" : "NO" );

    // Assumes dds_device initialization finished
    struct sensor_info
    {
        std::shared_ptr< dds_sensor_proxy > proxy;
        int sensor_index = 0;
    };
    std::map< std::string, sensor_info > sensor_name_to_info;

    // dds_streams bear stream type and index information, we add it to a dds_sensor_proxy mapped by a newly generated
    // unique ID. After the sensor initialization we get all the "final" profiles from formats-converter with type and
    // index but without IDs. We need to find the dds_stream that each profile was created from so we create a map from
    // type and index to dds_stream ID and index, because the dds_sensor_proxy holds a map from sidx to dds_stream. We
    // need both the ID from that map key and the stream itself (for intrinsics information)
    std::map< sid_index, sid_index > type_and_index_to_dds_stream_sidx;

    _dds_dev->foreach_stream(
        [&]( std::shared_ptr< realdds::dds_stream > const & stream )
        {
            auto & sensor_info = sensor_name_to_info[stream->sensor_name()];
            if( ! sensor_info.proxy )
            {
                // This is a new sensor we haven't seen yet
                sensor_info.proxy = create_sensor( stream->sensor_name() );
                sensor_info.sensor_index = add_sensor( sensor_info.proxy );
                assert( sensor_info.sensor_index == _software_sensors.size() );
                _software_sensors.push_back( sensor_info.proxy );
            }
            auto stream_type = to_rs2_stream_type( stream->type_string() );
            int index = get_index_from_stream_name( stream->name() );
            sid_index sidx( environment::get_instance().generate_stream_id(), index);
            sid_index type_and_index( stream_type, index );
            _stream_name_to_librs_stream[stream->name()]
                = std::make_shared< librealsense::stream >( stream_type, sidx.index );
            sensor_info.proxy->add_dds_stream( sidx, stream );
            _stream_name_to_owning_sensor[stream->name()] = sensor_info.proxy;
            type_and_index_to_dds_stream_sidx.insert( { type_and_index, sidx }  );
            LOG_DEBUG( sidx.to_string() << " " << stream->sensor_name() << " : " << stream->name() );

            software_sensor & sensor = get_software_sensor( sensor_info.sensor_index );
            auto video_stream = std::dynamic_pointer_cast< realdds::dds_video_stream >( stream );
            auto motion_stream = std::dynamic_pointer_cast< realdds::dds_motion_stream >( stream );
            auto & profiles = stream->profiles();
            auto const & default_profile = profiles[stream->default_profile_index()];
            for( auto & profile : profiles )
            {
                if( video_stream )
                {
                    auto added_stream_profile = sensor.add_video_stream(
                        to_rs2_video_stream( stream_type,
                                             sidx,
                                             std::static_pointer_cast< realdds::dds_video_stream_profile >( profile ),
                                             video_stream->get_intrinsics() ),
                        profile == default_profile );
                    _stream_name_to_profiles[stream->name()].push_back( added_stream_profile );  // for extrinsics
                }
                else if( motion_stream )
                {
                    auto added_stream_profile = sensor.add_motion_stream(
                        to_rs2_motion_stream( stream_type,
                                              sidx,
                                              std::static_pointer_cast< realdds::dds_motion_stream_profile >( profile ),
                                              motion_stream->get_intrinsics() ),
                        profile == default_profile );
                    _stream_name_to_profiles[stream->name()].push_back( added_stream_profile );  // for extrinsics
                }
            }

            auto & options = stream->options();
            for( auto & option : options )
            {
                sensor_info.proxy->add_option( option );
            }

            auto & recommended_filters = stream->recommended_filters();
            for( auto & filter_name : recommended_filters )
            {
                sensor_info.proxy->add_processing_block( filter_name );
            }
        } );  // End foreach_stream lambda

    for( auto & sensor_info : sensor_name_to_info )
    {
        sensor_info.second.proxy->initialization_done( dev->device_info().product_id, dev->device_info().product_line );

        // Set profile's ID based on the dds_stream's ID (index already set). Connect the profile to the extrinsics graph.
        for( auto & profile : sensor_info.second.proxy->get_stream_profiles() )
        {
            sid_index type_and_index( profile->get_stream_type(), profile->get_stream_index());
            
            auto & streams = sensor_info.second.proxy->streams();
            
            sid_index sidx = type_and_index_to_dds_stream_sidx[type_and_index];
            auto stream_iter = streams.find( sidx );
            if( stream_iter == streams.end() )
            {
                LOG_DEBUG( "Did not find dds_stream of profile (" << profile->get_stream_type() << ", "
                                                                  << profile->get_stream_index() << ")" );
                continue;
            }

            profile->set_unique_id( stream_iter->first.sid );
            set_profile_intrinsics( profile, stream_iter->second );

            _stream_name_to_profiles[stream_iter->second->name()].push_back( profile );  // For extrinsics
        }
    }

    if( _dds_dev->supports_metadata() )
    {
        _dds_dev->on_metadata_available(
            [this]( nlohmann::json && dds_md )
            {
                std::string stream_name = rsutils::json::get< std::string >( dds_md, stream_name_key );
                auto it = _stream_name_to_owning_sensor.find( stream_name );
                if( it != _stream_name_to_owning_sensor.end() )
                    it->second->handle_new_metadata( stream_name, std::move( dds_md ) );
            } );
    }

    // According to extrinsics_graph (in environment.h) we need 3 steps
    // 1. Register streams with extrinsics between them
    for( auto & from_stream : _stream_name_to_librs_stream )
    {
        for( auto & to_stream : _stream_name_to_librs_stream )
        {
            if( from_stream.first != to_stream.first )
            {
                const auto & dds_extr = _dds_dev->get_extrinsics( from_stream.first, to_stream.first );
                if( dds_extr )
                {
                    rs2_extrinsics extr = to_rs2_extrinsics( dds_extr );
                    environment::get_instance().get_extrinsics_graph().register_extrinsics( *from_stream.second,
                                                                                            *to_stream.second,
                                                                                            extr );
                }
            }
        }
    }
    // 2. Register all profiles
    for( auto & it : _stream_name_to_profiles )
    {
        for( auto profile : it.second )
        {
            environment::get_instance().get_extrinsics_graph().register_profile( *profile );
        }
    }
    // 3. Link profile to it's stream
    for( auto & it : _stream_name_to_librs_stream )
    {
        for( auto & profile : _stream_name_to_profiles[it.first] )
        {
            environment::get_instance().get_extrinsics_graph().register_same_extrinsics( *it.second, *profile );
        }
    }
    // TODO - need to register extrinsics group in dev?
}


int dds_device_proxy::get_index_from_stream_name( const std::string & name ) const
{
    int index = 0;

    auto delimiter_pos = name.find( '_' );
    if( delimiter_pos != std::string::npos )
    {
        std::string index_as_string = name.substr( delimiter_pos + 1, name.size() );
        index = std::stoi( index_as_string );
    }

    return index;
}


void dds_device_proxy::set_profile_intrinsics( std::shared_ptr< stream_profile_interface > & profile,
                                               const std::shared_ptr< realdds::dds_stream > & stream ) const
{
    if( auto video_stream = std::dynamic_pointer_cast< realdds::dds_video_stream >( stream ) )
    {
        set_video_profile_intrinsics( profile, video_stream );
    }
    else if( auto motion_stream = std::dynamic_pointer_cast< realdds::dds_motion_stream >( stream ) )
    {
        set_motion_profile_intrinsics( profile, motion_stream );
    }
}


void dds_device_proxy::set_video_profile_intrinsics( std::shared_ptr< stream_profile_interface > profile,
                                                     std::shared_ptr< realdds::dds_video_stream > stream ) const
{
    auto vsp = std::dynamic_pointer_cast< video_stream_profile >( profile );
    auto & stream_intrinsics = stream->get_intrinsics();
    auto it = std::find_if( stream_intrinsics.begin(),
                            stream_intrinsics.end(),
                            [vsp]( const realdds::video_intrinsics & intr )
                            { return vsp->get_width() == intr.width && vsp->get_height() == intr.height; } );

    if( it != stream_intrinsics.end() )  // Some profiles don't have intrinsics
    {
        rs2_intrinsics intr;
        intr.width = it->width;
        intr.height = it->height;
        intr.ppx = it->principal_point_x;
        intr.ppy = it->principal_point_y;
        intr.fx = it->focal_lenght_x;
        intr.fy = it->focal_lenght_y;
        intr.model = static_cast< rs2_distortion >( it->distortion_model );
        memcpy( intr.coeffs, it->distortion_coeffs.data(), sizeof( intr.coeffs ) );
        vsp->set_intrinsics( [intr]() { return intr; } );
    }
}


void dds_device_proxy::set_motion_profile_intrinsics( std::shared_ptr< stream_profile_interface > profile,
                                                      std::shared_ptr< realdds::dds_motion_stream > stream ) const
{
    auto msp = std::dynamic_pointer_cast< motion_stream_profile >( profile );
    auto & stream_intrinsics = stream->get_intrinsics();
    rs2_motion_device_intrinsic intr;
    memcpy( intr.data, stream_intrinsics.data.data(), sizeof( intr.data ) );
    memcpy( intr.noise_variances, stream_intrinsics.noise_variances.data(), sizeof( intr.noise_variances ) );
    memcpy( intr.bias_variances, stream_intrinsics.bias_variances.data(), sizeof( intr.bias_variances ) );
    msp->set_intrinsics( [intr]() { return intr; } );
}


std::shared_ptr< dds_sensor_proxy > dds_device_proxy::create_sensor( const std::string & sensor_name )
{
    if( sensor_name.compare( "RGB Camera" ) == 0 )
        return std::make_shared< dds_color_sensor_proxy >( sensor_name, this, _dds_dev );
    else if( sensor_name.compare( "Stereo Module" ) == 0 )
        return std::make_shared< dds_depth_sensor_proxy >( sensor_name, this, _dds_dev );

    return std::make_shared< dds_sensor_proxy >( sensor_name, this, _dds_dev );
}


}  // namespace librealsense