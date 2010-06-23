#include <vw/FileIO.h>
#include <vw/Image.h>
#include <vw/Math.h>
#include <vw/Cartography.h>
using namespace vw;

#include <boost/program_options.hpp>
#include <boost/type_traits.hpp>
#include <boost/utility/enable_if.hpp>
namespace po = boost::program_options;

// Function for highlighting spots of data
template<class PixelT>
class NotNoDataFunctor {
  typedef typename CompoundChannelType<PixelT>::type channel_type;
  channel_type m_nodata;
  typedef ChannelRange<channel_type> range_type;
public:
  NotNoDataFunctor( channel_type nodata ) : m_nodata(nodata) {}

  template <class Args> struct result {
    typedef channel_type type;
  };

  inline channel_type operator()( channel_type const& val ) const {
    return val != m_nodata ? range_type::max() : range_type::min();
  }
};

template <class ImageT, class NoDataT>
UnaryPerPixelView<ImageT,UnaryCompoundFunctor<NotNoDataFunctor<typename ImageT::pixel_type>, typename ImageT::pixel_type>  >
inline notnodata( ImageViewBase<ImageT> const& image, NoDataT nodata ) {
  typedef UnaryCompoundFunctor<NotNoDataFunctor<typename ImageT::pixel_type>, typename ImageT::pixel_type> func_type;
  func_type func( nodata );
  return UnaryPerPixelView<ImageT,func_type>( image.impl(), func );
}

// Linear Transfer Function
class LinearTransFunc : public vw::UnaryReturnSameType {
public:
  LinearTransFunc() {}

  template <class ArgT>
  ArgT operator()( ArgT const& value ) const { return value; }
};

// Cosine Transfer Function (this tracks 180 degrees with level at high and low)
template <class PixelT>
class CosineTransFunc : public vw::UnaryReturnSameType {
  typedef typename CompoundChannelType<PixelT>::type channel_type;
  typedef ChannelRange<channel_type> range_type;
public:
  CosineTransFunc() {}

  template <class ArgT>
  inline typename boost::enable_if<typename boost::is_floating_point<ArgT>,ArgT>::type
  operator()( ArgT const& value ) const {
    return range_type::max()*((1.0-cos(value/float(range_type::max())*M_PI))/2.0);
  }

  template <class ArgT>
  inline typename boost::disable_if<typename boost::is_floating_point<ArgT>,ArgT>::type
  operator()( ArgT const& value ) const {
    ArgT result = range_type::max()*((1.0-cos(float(value)/float(range_type::max())*M_PI))/2.0);
    if ( result == 0 && value != 0 )
      result = 1;
    return result;
  }
};

// 90 degree Cosine transfer function ( high slope at beginning and low slope at end )
template <class PixelT>
class Cosine90TransFunc : public vw::UnaryReturnSameType {
  typedef typename CompoundChannelType<PixelT>::type channel_type;
  typedef ChannelRange<channel_type> range_type;
public:
  Cosine90TransFunc() {}

  template <class ArgT>
  inline typename boost::enable_if<typename boost::is_floating_point<ArgT>,ArgT>::type
  operator()( ArgT const& value ) const {
    return range_type::max()*(-cos(value/float(range_type::max())*(M_PI/2.0) + M_PI/2.0));
  }

  template <class ArgT>
  inline typename boost::disable_if<typename boost::is_floating_point<ArgT>,ArgT>::type
  operator()( ArgT const& value ) const {
    ArgT result = range_type::max()*(-cos(float(value)/float(range_type::max())*(M_PI/2.0) + M_PI/2.0));
    if ( result == 0 && value != 0 )
      result = 1;
    return result;
  }
};

// Function to zip 2 images into content and alpha image
template <class Arg1T, class Arg2T>
struct CreateAlphaFunc: public ReturnFixedType<typename PixelWithAlpha<Arg1T>::type > {
  typedef typename PixelWithAlpha<Arg1T>::type result_type;
  inline result_type operator()(Arg1T const& arg1,
                                Arg2T const& arg2 ) const {
    result_type t(arg1);
    t.a() = arg2;
    return t;
  }
};

template <class Image1T, class Image2T>
inline BinaryPerPixelView<Image1T, Image2T, CreateAlphaFunc<typename Image1T::pixel_type, typename Image2T::pixel_type> >
create_alpha( ImageViewBase<Image1T> const& image1,
              ImageViewBase<Image2T> const& image2 ) {
  typedef CreateAlphaFunc<typename Image1T::pixel_type, typename Image2T::pixel_type> func_type;
  return BinaryPerPixelView<Image1T, Image2T, func_type >(image1.impl(), image2.impl(), func_type());
}

struct Options {
  Options() : nodata(-1), feather_length(0) {}
  // Input
  std::vector<std::string> input_files;

  // Settings
  double nodata;
  int feather_length;
  std::string filter;
};

// Operation code for data that uses nodata
template <class PixelT>
void grassfire_nodata( Options& opt,
                       std::string input,
                       std::string output ) {
  cartography::GeoReference georef;
  cartography::read_georeference(georef, input);
  DiskImageView<PixelT> input_image(input);
  ImageView<int32> distance = grassfire(notnodata(input_image,opt.nodata));
  int32 max = opt.feather_length;
  if ( max < 1 )
    max = max_pixel_value( distance );
  vw_out() << "\tMax distance: " << max << "\n";
  typedef typename CompoundChannelType<PixelT>::type inter_type;
  typedef ChannelRange<typename CompoundChannelType<PixelT>::type> range_type;
  ImageViewRef<inter_type> norm_dist = pixel_cast<inter_type>(range_type::max()*clamp(pixel_cast<float>(distance)/float(max)));
  ImageViewRef<typename PixelWithAlpha<PixelT>::type> result;

  if ( opt.filter == "linear" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,LinearTransFunc()));
  } else if ( opt.filter == "cosine" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,CosineTransFunc<inter_type>()));
  } else if ( opt.filter == "cosine90" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,Cosine90TransFunc<inter_type>()));
  } else {
    vw_throw( ArgumentErr() << "Unknown transfer function " << opt.filter );
  }

  cartography::write_georeferenced_image(output, result, georef,
                                         TerminalProgressCallback("tools.grassfirealpha","Writing:"));
}

// Same as above but modified for alpha input
template <class PixelT>
void grassfire_alpha( Options& opt,
                      std::string input,
                      std::string output ) {
  cartography::GeoReference georef;
  cartography::read_georeference(georef, input);
  DiskImageView<PixelT> input_image(input);
  ImageView<int32> distance = grassfire(apply_mask(invert_mask(alpha_to_mask(input_image)),1));
  int32 max = opt.feather_length;
  if ( max < 1 )
    max = max_pixel_value(distance);
  vw_out() << "\tMax distance: " << max << "\n";
  typedef typename CompoundChannelType<PixelT>::type inter_type;
  typedef ChannelRange<typename CompoundChannelType<PixelT>::type> range_type;
  ImageViewRef<inter_type> norm_dist = pixel_cast<inter_type>(range_type::max()*clamp(pixel_cast<float>(distance)/float(max)));
  ImageViewRef<PixelT> result;

  if ( opt.filter == "linear" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,LinearTransFunc()));
  } else if ( opt.filter == "cosine" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,CosineTransFunc<inter_type>()));
  } else if ( opt.filter == "cosine90" ) {
    result = create_alpha(input_image,per_pixel_filter(norm_dist,Cosine90TransFunc<inter_type>()));
  } else {
    vw_throw( ArgumentErr() << "Unknown transfer function " << opt.filter );
  }

  cartography::write_georeferenced_image(output, result, georef,
                                         TerminalProgressCallback("tools.grassfirealpha","Writing:"));
}

// Handling input
void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description general_options("");
  general_options.add_options()
    ("nodata-value", po::value(&opt.nodata), "Value that is nodata in the input image. Not used if input has alpha.")
    ("feather-length,f", po::value(&opt.feather_length)->default_value(0), "Length in pixels to feather from an edge. Default size of zero is to feather to maximum distance in image.")
    ("transfer-func,t", po::value(&opt.filter)->default_value("cosine"), "Transfer function to used for alpha. [linear, cosine, cosine90]")
    ("help,h", "Display this help message");

  po::options_description positional("");
  positional.add_options()
    ("input-files", po::value<std::vector<std::string> >(&opt.input_files));

  po::positional_options_description positional_desc;
  positional_desc.add("input-files", -1);

  po::options_description all_options;
  all_options.add(general_options).add(positional);

  po::variables_map vm;
  try {
    po::store( po::command_line_parser( argc, argv ).options(all_options).positional(positional_desc).run(), vm );
    po::notify( vm );
  } catch (po::error &e) {
    vw_throw( ArgumentErr() << "Error parsing input:\n\t"
              << e.what() << general_options );
  }

  std::ostringstream usage;
  usage << "Usage: " << argv[0] << " [options] <image-files>\n";
  boost::to_lower( opt.filter );

  if ( vm.count("help") )
    vw_throw( ArgumentErr() << usage.str() << general_options );
  if ( opt.input_files.empty() )
    vw_throw( ArgumentErr() << "Missing input files!\n"
              << usage.str() << general_options );
}

int main( int argc, char *argv[] ) {

  Options opt;
  try {
    handle_arguments( argc, argv, opt );

    BOOST_FOREACH( const std::string& input, opt.input_files ) {

      // Determining the format of the input
      DiskImageResource *rsrc = DiskImageResource::open(input);
      ChannelTypeEnum channel_type = rsrc->channel_type();
      PixelFormatEnum pixel_format = rsrc->pixel_format();
      delete rsrc;

      vw_out() << "Loading: " << input << "\n";
      size_t pt_idx = input.rfind(".");
      std::string output = input.substr(0,pt_idx)+"_grass" +
        input.substr(pt_idx,input.size()-pt_idx);

      switch (pixel_format) {
      case VW_PIXEL_GRAY:
        switch (channel_type) {
        case VW_CHANNEL_UINT8:
          grassfire_nodata<PixelGray<uint8> >(opt,input,output); break;
        case VW_CHANNEL_INT16:
          grassfire_nodata<PixelGray<int16> >(opt,input,output); break;
        case VW_CHANNEL_UINT16:
          grassfire_nodata<PixelGray<uint16> >(opt,input,output); break;
        default:
          grassfire_nodata<PixelGray<float32> >(opt,input,output); break;
        }
        break;
      case VW_PIXEL_GRAYA:
        switch (channel_type) {
        case VW_CHANNEL_UINT8:
          grassfire_alpha<PixelGrayA<uint8> >(opt,input,output); break;
        case VW_CHANNEL_INT16:
          grassfire_alpha<PixelGrayA<int16> >(opt,input,output); break;
        case VW_CHANNEL_UINT16:
          grassfire_alpha<PixelGrayA<uint16> >(opt,input,output); break;
        default:
          grassfire_alpha<PixelGrayA<float32> >(opt,input,output); break;
        }
        break;
      case VW_PIXEL_RGB:
        switch (channel_type) {
        case VW_CHANNEL_UINT8:
          grassfire_nodata<PixelRGB<uint8> >(opt,input,output); break;
        case VW_CHANNEL_INT16:
          grassfire_nodata<PixelRGB<int16> >(opt,input,output); break;
        case VW_CHANNEL_UINT16:
          grassfire_nodata<PixelRGB<uint16> >(opt,input,output); break;
        default:
          grassfire_nodata<PixelRGB<float32> >(opt,input,output); break;
        }
        break;
      default:
        switch (channel_type) {
        case VW_CHANNEL_UINT8:
          grassfire_alpha<PixelRGBA<uint8> >(opt,input,output); break;
        case VW_CHANNEL_INT16:
          grassfire_alpha<PixelRGBA<int16> >(opt,input,output); break;
        case VW_CHANNEL_UINT16:
          grassfire_alpha<PixelRGBA<uint16> >(opt,input,output); break;
        default:
          grassfire_alpha<PixelRGBA<float32> >(opt,input,output); break;
        }
        break;
      }
    } // end FOREACH
  } catch ( const ArgumentErr& e ) {
    vw_out() << e.what() << std::endl;
    return 1;
  } catch ( const Exception& e ) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
}
