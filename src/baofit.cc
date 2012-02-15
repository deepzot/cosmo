// Created 31-Jan-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

#include "cosmo/cosmo.h"
#include "likely/likely.h"
// the following are not part of the public API, so not included by likely.h
#include "likely/MinuitEngine.h"
#include "likely/EngineRegistry.h"

#include "Minuit2/MnUserParameters.h"
#include "Minuit2/FunctionMinimum.h"
#include "Minuit2/MnPrint.h"
#include "Minuit2/MnStrategy.h"
#include "Minuit2/MnMigrad.h"
#include "Minuit2/MnMinos.h"
#include "Minuit2/MnContours.h"

#include "boost/program_options.hpp"
#include "boost/bind.hpp"
#include "boost/ref.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/regex.hpp"
#include "boost/format.hpp"
#include "boost/foreach.hpp"

#include <fstream>
#include <iostream>
#include <cmath>
#include <cassert>
#include <vector>
#include <algorithm>

// Declare bindings to BLAS,LAPACK routines we need
extern "C" {
    // http://www.netlib.org/lapack/double/dpptrf.f
    void dpptrf_(char *uplo, int *n, double *ap, int *info);
    // http://www.netlib.org/lapack/double/dpptri.f
    void dpptri_(char *uplo, int *n, double *ap, int *info);
    // http://netlib.org/blas/dspmv.f
    void dspmv_(char *uplo, int *n, double *alpha, double *ap,
        double *x, int *incx, double *beta, double *y, int *incy);
}

namespace lk = likely;
namespace po = boost::program_options;

class BaoFitPower {
public:
    BaoFitPower(cosmo::PowerSpectrumPtr fiducial, cosmo::PowerSpectrumPtr nowiggles)
    : _fiducial(fiducial), _nowiggles(nowiggles) {
        assert(fiducial);
        assert(nowiggles);
        setAmplitude(1);
        setScale(1);
        setSigma(0);
    }
    // Setter methods
    void setAmplitude(double value) { _amplitude = value; }
    void setScale(double value) { _scale = value; double tmp(value*value); _scale4 = tmp*tmp; }
    void setSigma(double value) { _sigma = value; _sigma2 = value*value; }
    // Returns the hybrid power k^3/(2pi^2) P(k) at the specified wavenumber k in Mpc/h.
    double operator()(double k) const {
        double ak(k/_scale), smooth(std::exp(-ak*ak*_sigma2/2));
        double fiducialPower = (*_fiducial)(ak), nowigglesPower = (*_nowiggles)(ak);
        return _scale4*(_amplitude*smooth*(fiducialPower - nowigglesPower) + nowigglesPower);
    }
private:
    double _amplitude, _scale, _scale4, _sigma, _sigma2;
    cosmo::PowerSpectrumPtr _fiducial, _nowiggles;
}; // BaoFitPower

typedef boost::shared_ptr<BaoFitPower> BaoFitPowerPtr;

class AbsBinning {
public:
    AbsBinning() { }
    virtual ~AbsBinning() { }
    // Returns the bin index [0,nBins-1] or else -1.
    virtual int getBinIndex(double value) const = 0;
    // Returns the total number of bins.
    virtual int getNBins() const = 0;
    // Returns the full width of the specified bin.
    virtual double getBinSize(int index) const = 0;
    // Returns the lower bound of the specified bin. Use index=nbins for the upper bound of the last bin.
    virtual double getBinLowEdge(int index) const = 0;
    // Returns the midpoint value of the specified bin.
    virtual double getBinCenter(int index) const { return getBinLowEdge(index) + 0.5*getBinSize(index); }
    // Dumps this binning to the specified output stream in a standard format.
    void dump(std::ostream &os) const {
        int nbins(getNBins());
        os << nbins;
        for(int bin = 0; bin <= nbins; ++bin) os << ' ' << getBinLowEdge(bin);
        os << std::endl;
    }
}; // AbsBinning

typedef boost::shared_ptr<const AbsBinning> AbsBinningPtr;

class UniformBinning : public AbsBinning {
public:
    UniformBinning(int nBins, double lowEdge, double binSize)
    : _nBins(nBins), _lowEdge(lowEdge), _binSize(binSize) {
        assert(nBins > 0);
        assert(binSize > 0);
    }
    virtual ~UniformBinning() { }
    // Returns the bin index [0,nBins-1] or else -1.
    virtual int getBinIndex(double value) const {
        int bin = std::floor((value - _lowEdge)/_binSize);
        assert(bin >= 0 && bin < _nBins);
        return bin;
    }
    // Returns the total number of bins.
    virtual int getNBins() const { return _nBins; }
    // Returns the full width of the specified bin.
    virtual double getBinSize(int index) const { return _binSize; }
    // Returns the lower bound of the specified bin. Use index=nbins for the upper bound of the last bin.
    virtual double getBinLowEdge(int index) const {
        assert(index >= 0 && index <= _nBins);
        return _lowEdge + index*_binSize;
    }
private:
    int _nBins;
    double _lowEdge, _binSize;
}; // UniformBinning

class VariableBinning : public AbsBinning {
public:
    VariableBinning(std::vector<double> &binEdge) :
    _binEdge(binEdge) {
        // should check that edges are increasing...
    }
    virtual ~VariableBinning() { }
    virtual int getBinIndex(double value) const {
        // should use bisection for this, and cache the last bin found...
        if(value < _binEdge[0]) return -1; // underflow
        for(int bin = 1; bin < _binEdge.size(); ++bin) if(value < _binEdge[bin]) return bin-1;
        return -1; // overflow
    }
    virtual int getNBins() const { return _binEdge.size()-1; }
    virtual double getBinSize(int index) const {
        assert(index >= 0 && index < _binEdge.size()-1);
        return _binEdge[index+1] - _binEdge[index];
    }
    virtual double getBinLowEdge(int index) const {
        assert(index >= 0 && index < _binEdge.size());
        return _binEdge[index];
    }
protected:
    VariableBinning() { }
    std::vector<double> _binEdge;
}; // VariableBinning

class TwoStepBinning : public VariableBinning {
public:
    TwoStepBinning(int nBins, double breakpoint, double dlog, double dlin, double eps = 1e-3) {
        assert(breakpoint > 0 && dlog > 0 && dlin > 0 && eps > 0);
        // first bin is centered on zero with almost zero width
        _binEdge.push_back(-eps*dlin);
        _binEdge.push_back(+eps*dlin);
        _binCenter.push_back(0);
        // next bins are uniformly spaced up to the breakpoint
        int nUniform = std::floor(breakpoint/dlin);
        for(int k = 1; k <= nUniform; ++k) {
            _binEdge.push_back(k*dlin);
            _binCenter.push_back((k-0.5)*dlin);
        }
        // remaining bins are logarithmically spaced, with log-weighted bin centers.
        double ratio = std::log((breakpoint+dlog)/breakpoint);
        for(int k = 1; k < nBins-nUniform; ++k) {
            _binEdge.push_back(breakpoint*std::exp(ratio*k));
            _binCenter.push_back(breakpoint*std::exp(ratio*(k-0.5)));
        }
    }
    virtual ~TwoStepBinning() { }
    virtual double getBinCenter(int index) const {
        assert(index >= 0 && index < _binCenter.size());
        return _binCenter[index];
    }
private:
    std::vector<double> _binCenter;
}; // TwoStepBinning

class LyaData {
public:
    LyaData(AbsBinningPtr logLambdaBinning, AbsBinningPtr separationBinning, AbsBinningPtr redshiftBinning,
    cosmo::AbsHomogeneousUniversePtr cosmology) : _cosmology(cosmology), _logLambdaBinning(logLambdaBinning),
    _separationBinning(separationBinning), _redshiftBinning(redshiftBinning),
    _dataFinalized(false), _covarianceFinalized(false)
    {
        assert(logLambdaBinning);
        assert(separationBinning);
        assert(redshiftBinning);
        assert(cosmology);
        _nsep = separationBinning->getNBins();
        _nz = redshiftBinning->getNBins();
        _nBinsTotal = logLambdaBinning->getNBins()*_nsep*_nz;
        _initialized.resize(_nBinsTotal,false);
        _arcminToRad = 4*std::atan(1)/(60.*180.);
    }
    void addData(double value, double logLambda, double separation, double redshift) {
        // Lookup which (ll,sep,z) bin we are in.
        int logLambdaBin(_logLambdaBinning->getBinIndex(logLambda)),
            separationBin(_separationBinning->getBinIndex(separation)),
            redshiftBin(_redshiftBinning->getBinIndex(redshift));
        int index = (logLambdaBin*_nsep + separationBin)*_nz + redshiftBin;
        // Check that input (ll,sep,z) values correspond to bin centers.
        assert(std::fabs(logLambda-_logLambdaBinning->getBinCenter(logLambdaBin)) < 1e-6);
        assert(std::fabs(separation-_separationBinning->getBinCenter(separationBin)) < 1e-6);
        assert(std::fabs(redshift-_redshiftBinning->getBinCenter(redshiftBin)) < 1e-6);
        // Check that we have not already filled this bin.
        assert(!_initialized[index]);
        // Remember this bin.
        _data.push_back(value);
        _initialized[index] = true;
        _index.push_back(index);
        // Calculate and save model observables for this bin.
        double r3d,mu,ds(_separationBinning->getBinSize(separationBin));
        transform(logLambda,separation,redshift,ds,r3d,mu);
        _r3d.push_back(r3d);
        _mu.push_back(mu);
    }
    void finalizeData() {
        int nData = getNData();
        int nCov = (nData*(nData+1))/2;
        _cov.resize(nCov,0);
        _hasCov.resize(nCov,false);
        _dataFinalized = true;
    }
    void transform(double ll, double sep, double z, double ds, double &r3d, double &mu) const {
        double ratio(std::exp(0.5*ll)),zp1(z+1);
        double z1(zp1/ratio-1), z2(zp1*ratio-1);
        double drLos = _cosmology->getLineOfSightComovingDistance(z2) -
            _cosmology->getLineOfSightComovingDistance(z1);
        // Calculate the geometrically weighted mean separation of this bin as
        // Integral[s^2,{s,smin,smax}]/Integral[s,{s,smin,smax}] = s + ds^2/(12*s)
        double swgt = sep + (ds*ds/12)/sep;
        double drPerp = _cosmology->getTransverseComovingScale(z)*(swgt*_arcminToRad);
        double rsq = drLos*drLos + drPerp*drPerp;
        r3d = std::sqrt(rsq);
        mu = std::abs(drLos)/r3d;
    }
    void addCovariance(int i, int j, double value) {
        int row,col;
         // put into upper-diagonal form col >= row
        if(i >= j) {
            col = i; row = j;
        }
        else {
            row = i; col = j;
        }
        assert(_dataFinalized);
        assert(row >= 0 && col >= 0 && col < getNData());
        assert(col > row || value > 0); // diagonal elements must be positive
        int index(row+(col*(col+1))/2); // see http://www.netlib.org/lapack/lug/node123.html
        assert(_hasCov[index] == false);
        _cov[index] = value;
        _hasCov[index] = true;
    }
    void finalizeCovariance() {
        assert(_dataFinalized);
        _icov = _cov; // element-by-element copy
        char uplo('U');
        int info(0),ndata(getNData());
        dpptrf_(&uplo,&ndata,&_icov[0],&info); // Cholesky decompose
        if(0 != info) std::cout << "Cholesky error: info = " << info << std::endl;
        assert(0 == info);
        dpptri_(&uplo,&ndata,&_icov[0],&info); // Calculate inverse
        if(0 != info) std::cout << "Inverse error: info = " << info << std::endl;
        assert(0 == info);
        _covarianceFinalized = true;
    }
    int getSize() const { return _nBinsTotal; }
    int getNData() const { return _data.size(); }
    int getNCov() const { return (int)std::count(_hasCov.begin(),_hasCov.end(),true); }
    int getIndex(int k) const { return _index[k]; }
    double getData(int k) const { return _data[k]; }
    double getVariance(int k) const { return _cov[(k*(k+3))/2]; }
    double getRadius(int k) const { return _r3d[k]; }
    double getCosAngle(int k) const { return _mu[k]; }
    double getRedshift(int k) const { return _redshiftBinning->getBinCenter(_index[k] % _nz); }
    AbsBinningPtr getLogLambdaBinning() const { return _logLambdaBinning; }
    AbsBinningPtr getSeparationBinning() const { return _separationBinning; }
    AbsBinningPtr getRedshiftBinning() const { return _redshiftBinning; }
    double calculateChiSquare(std::vector<double> &delta) {
        std::vector<double>(delta.size(),0).swap(_icovDelta); // zero _icovDelta
        char uplo('U');
        double alpha(1),beta(0);
        int info(0),incr(1),ndata(getNData());
        // See http://netlib.org/blas/dspmv.f
        dspmv_(&uplo,&ndata,&alpha,&_icov[0],&delta[0],&incr,&beta,&_icovDelta[0],&incr);
        double chi2(0);
        for(int k = 0; k < ndata; ++k) {
            chi2 += delta[k]*_icovDelta[k];
        }
        return chi2;
    }
private:
    AbsBinningPtr _logLambdaBinning, _separationBinning, _redshiftBinning;
    cosmo::AbsHomogeneousUniversePtr _cosmology;
    std::vector<double> _data, _cov, _icov, _r3d, _mu, _icovDelta;
    std::vector<bool> _initialized, _hasCov;
    std::vector<int> _index;
    int _ndata,_nsep,_nz,_nBinsTotal;
    double _arcminToRad;
    bool _dataFinalized, _covarianceFinalized;
}; // LyaData

typedef boost::shared_ptr<LyaData> LyaDataPtr;

class LyaBaoModel {
public:
    LyaBaoModel(std::string const &fiducialName, std::string const &nowigglesName, double zref)
    : _zref(zref) {
        boost::format fileName("%s.%d.dat");
        _fid0 = load(boost::str(fileName % fiducialName % 0));
        _fid2 = load(boost::str(fileName % fiducialName % 2));
        _fid4 = load(boost::str(fileName % fiducialName % 4));
        _nw0 = load(boost::str(fileName % nowigglesName % 0));
        _nw2 = load(boost::str(fileName % nowigglesName % 2));
        _nw4 = load(boost::str(fileName % nowigglesName % 4));
        cosmo::CorrelationFunctionPtr fid0(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_fid0,_1)));
        cosmo::CorrelationFunctionPtr fid2(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_fid2,_1)));
        cosmo::CorrelationFunctionPtr fid4(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_fid4,_1)));
        cosmo::CorrelationFunctionPtr nw0(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_nw0,_1)));
        cosmo::CorrelationFunctionPtr nw2(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_nw2,_1)));
        cosmo::CorrelationFunctionPtr nw4(new cosmo::CorrelationFunction(boost::bind(
            &lk::Interpolator::operator(),_nw4,_1)));
        _fid.reset(new cosmo::RsdCorrelationFunction(fid0,fid2,fid4));
        _nw.reset(new cosmo::RsdCorrelationFunction(nw0,nw2,nw4));
    }
    double evaluate(double r, double mu, double z, lk::Parameters const &p) const {
        double alpha(p[0]), bias(p[1]), beta(p[2]), ampl(p[3]), scale(p[4]);
        double a1(p[5]), a2(p[6]), a3(p[7]);
        double zfactor = std::pow((1+z)/(1+_zref),alpha);
        _fid->setDistortion(beta);
        _nw->setDistortion(beta);
        double fid((*_fid)(r*scale,mu)), nw((*_nw)(r*scale,mu)); // scale cancels in mu
        double xi = ampl*(fid-nw)+nw;
        double broadband = 1e-1*a1/(r*r) + 1e-3*a2/r + 1e-5*a3;
        return bias*bias*zfactor*xi + broadband;
    }
private:
    lk::InterpolatorPtr load(std::string const &fileName) {
        std::vector<std::vector<double> > columns(2);
        std::ifstream in(fileName.c_str());
        lk::readVectors(in,columns);
        in.close();
        lk::InterpolatorPtr iptr(new lk::Interpolator(columns[0],columns[1],"cspline"));
        return iptr;
    }
    double _zref, _growth;
    lk::InterpolatorPtr _fid0, _fid2, _fid4, _nw0, _nw2, _nw4;
    boost::scoped_ptr<cosmo::RsdCorrelationFunction> _fid, _nw;
}; // LyaBaoModel

typedef boost::shared_ptr<LyaBaoModel> LyaBaoModelPtr;

typedef std::pair<double,double> ContourPoint;
typedef std::vector<ContourPoint> ContourPoints;

class Parameter {
public:
    Parameter(std::string const &name, double value, bool floating = false)
    : _name(name), _value(value), _floating(floating)
    { }
    void fix(double value) {
        _value = value;
        _floating = false;
    }
    void setValue(double value) { _value = value; }
    bool isFloating() const { return _floating; }
    double getValue() const { return _value; }
    std::string getName() const { return _name; }
private:
    std::string _name;
    double _value;
    bool _floating;
}; // Parameter

class LyaBaoLikelihood {
public:
    LyaBaoLikelihood(LyaDataPtr data, LyaBaoModelPtr model, double rmin, double rmax,
    bool fixBao, bool noBBand)
    : _data(data), _model(model), _rmin(rmin), _rmax(rmax), _errorScale(1) {
        assert(data);
        assert(model);
        assert(rmax > rmin);
        _params.push_back(Parameter("Alpha",3.8,true));
        _params.push_back(Parameter("Bias",0.17,true));
        _params.push_back(Parameter("Beta",1.0,true));
        _params.push_back(Parameter("BAO Ampl",1,!fixBao));
        _params.push_back(Parameter("BAO Scale",1,!fixBao));
        _params.push_back(Parameter("BB a1",0,!noBBand));
        _params.push_back(Parameter("BB a2",0,false));
        _params.push_back(Parameter("BB a3",0,false));
    }
    void setErrorScale(double scale) {
        assert(scale > 0);
        _errorScale = scale;
    }
    double operator()(lk::Parameters const &params) {
        // Loop over the dataset bins.
        int ndata(_data->getNData());
        std::vector<double> delta(ndata,0);
        for(int k= 0; k < _data->getNData(); ++k) {
            double r = _data->getRadius(k);
            if(r < _rmin || r > _rmax) continue;
            double mu = _data->getCosAngle(k);
            double z = _data->getRedshift(k);
            double obs = _data->getData(k);
            double pred = _model->evaluate(r,mu,z,params);
            delta[k] = obs - pred;
        }
        // UP=0.5 is already hardcoded so we need a factor of 2 here since we are
        // calculating a chi-square. Apply an additional factor of _errorScale to
        // allow different error contours to be calculated.
        return 0.5*_data->calculateChiSquare(delta)/_errorScale;
    }
    int getNPar() const { return _params.size(); }
    void initialize(lk::MinuitEngine::StatePtr initialState) {
        BOOST_FOREACH(Parameter const &param, _params) {
            double value(param.getValue());
            if(param.isFloating()) {
                double error = (0 == value) ? 0.1 : 0.1*std::fabs(value);
                initialState->Add(param.getName(),value,error);
            }
            else {
                initialState->Add(param.getName(),value,0);
                initialState->Fix(param.getName());
            }
        }
    }
    void dump(std::string const &filename, lk::Parameters const &params,
    std::vector<ContourPoints> const &contourData, int modelBins) {
        std::ofstream out(filename.c_str());
        // Dump binning info first
        AbsBinningPtr llbins(_data->getLogLambdaBinning()), sepbins(_data->getSeparationBinning()),
            zbins(_data->getRedshiftBinning());
        llbins->dump(out);
        sepbins->dump(out);
        zbins->dump(out);
        // Dump the number of data bins, the number of model bins, and the number of contour points.
        int ncontour = (0 == contourData.size()) ? 0 : contourData[0].size();
        out << _data->getNData() << ' ' << modelBins << ' ' << ncontour << std::endl;
        // Dump the number of parameters and their best-fit values.
        out << params.size();
        BOOST_FOREACH(double const &pValue, params) out << ' ' << pValue;
        out << std::endl;
        // Dump binned data and most recent pulls.
        for(int k= 0; k < _data->getNData(); ++k) {
            double r = _data->getRadius(k);
            double mu = _data->getCosAngle(k);
            double z = _data->getRedshift(k);
            double obs = _data->getData(k);
            double pull = 0;
            if(r >= _rmin && r <= _rmax) {
                double var = _data->getVariance(k);
                double pred = _model->evaluate(r,mu,z,params);
                pull = (obs-pred)/std::sqrt(var);
            }
            int index = _data->getIndex(k);
            out << index << ' ' << obs << ' ' << pull << std::endl;
        }
        // Dump high-resolution uniformly-binned model calculation.
        // Calculate and dump the model binning limits.
        double sepMin = sepbins->getBinLowEdge(0), sepMax = sepbins->getBinLowEdge(sepbins->getNBins());
        UniformBinning sepModel(modelBins,sepMin,(sepMax-sepMin)/(modelBins-1.));
        double llMin = llbins->getBinLowEdge(0), llMax = llbins->getBinLowEdge(llbins->getNBins());
        UniformBinning llModel(modelBins,llMin,(llMax-llMin)/(modelBins-1.));
        double r,mu;
        for(int iz = 0; iz < zbins->getNBins(); ++iz) {
            double z = zbins->getBinCenter(iz);
            for(int isep = 0; isep < modelBins; ++isep) {
                double sep = sepModel.getBinCenter(isep);
                double ds = sepModel.getBinSize(isep);
                for(int ill = 0; ill < modelBins; ++ill) {
                    double ll = llModel.getBinCenter(ill);
                    _data->transform(ll,sep,z,ds,r,mu);
                    double pred = _model->evaluate(r,mu,z,params);
                    out << r << ' ' << pred << std::endl;
                }
            }
        }
        // Dump 2-parameter contours if we have any.
        if(ncontour) {
            BOOST_FOREACH(ContourPoints const &points, contourData) {
                BOOST_FOREACH(ContourPoint const &point, points) {
                    out << point.first << ' ' << point.second << std::endl;
                }
            }
        }
        out.close();
    }
private:
    LyaDataPtr _data;
    LyaBaoModelPtr _model;
    std::vector<Parameter> _params;
    double _rmin, _rmax, _errorScale;
}; // LyaBaoLikelihood

int main(int argc, char **argv) {
    
    // Configure command-line option processing
    po::options_description cli("BAO fitting");
    double OmegaLambda,OmegaMatter,zref,minll,dll,dll2,minsep,dsep,minz,dz,rmin,rmax;
    int nll,nsep,nz,ncontour,modelBins;
    std::string fiducialName,nowigglesName,dataName,dumpName;
    cli.add_options()
        ("help,h", "Prints this info and exits.")
        ("verbose", "Prints additional information.")
        ("omega-lambda", po::value<double>(&OmegaLambda)->default_value(0.734),
            "Present-day value of OmegaLambda.")
        ("omega-matter", po::value<double>(&OmegaMatter)->default_value(0.266),
            "Present-day value of OmegaMatter or zero for 1-OmegaLambda.")
        ("fiducial", po::value<std::string>(&fiducialName)->default_value(""),
            "Fiducial correlation functions will be read from <name>.<ell>.dat with ell=0,2,4.")
        ("nowiggles", po::value<std::string>(&nowigglesName)->default_value(""),
            "No-wiggles correlation functions will be read from <name>.<ell>.dat with ell=0,2,4.")
        ("zref", po::value<double>(&zref)->default_value(2.25),
            "Reference redshift.")
        ("rmin", po::value<double>(&rmin)->default_value(0),
            "Minimum 3D comoving separation (Mpc/h) to use in fit.")
        ("rmax", po::value<double>(&rmax)->default_value(200),
            "Maximum 3D comoving separation (Mpc/h) to use in fit.")
        ("data", po::value<std::string>(&dataName)->default_value(""),
            "3D covariance data will be read from <data>.params and <data>.cov")
        ("minll", po::value<double>(&minll)->default_value(0.0002),
            "Minimum log(lam2/lam1).")
        ("dll", po::value<double>(&dll)->default_value(0.004),
            "log(lam2/lam1) binsize.")
        ("dll2", po::value<double>(&dll2)->default_value(0),
            "log(lam2/lam1) second binsize parameter for two-step binning.")
        ("nll", po::value<int>(&nll)->default_value(14),
            "Maximum number of log(lam2/lam1) bins.")
        ("minsep", po::value<double>(&minsep)->default_value(0),
            "Minimum separation in arcmins.")
        ("dsep", po::value<double>(&dsep)->default_value(10),
            "Separation binsize in arcmins.")
        ("nsep", po::value<int>(&nsep)->default_value(14),
            "Maximum number of separation bins.")
        ("minz", po::value<double>(&minz)->default_value(1.7),
            "Minimum redshift.")
        ("dz", po::value<double>(&dz)->default_value(1.0),
            "Redshift binsize.")
        ("nz", po::value<int>(&nz)->default_value(2),
            "Maximum number of redshift bins.")
        ("dump", po::value<std::string>(&dumpName)->default_value(""),
            "Filename for dumping fit results.")
        ("ncontour",po::value<int>(&ncontour)->default_value(40),
            "Number of contour points to calculate in BAO parameters.")
        ("model-bins", po::value<int>(&modelBins)->default_value(200),
            "Number of high-resolution uniform bins to use for dumping best fit model.")
        ("minos", "Runs MINOS to improve error estimates.")
        ("fix-bao", "Fix BAO scale and amplitude parameters.")
        ("no-bband", "Do not add any broadband contribution to the correlation function.")
        ;

    // Do the command line parsing now.
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, cli), vm);
        po::notify(vm);
    }
    catch(std::exception const &e) {
        std::cerr << "Unable to parse command line options: " << e.what() << std::endl;
        return -1;
    }
    if(vm.count("help")) {
        std::cout << cli << std::endl;
        return 1;
    }
    bool verbose(vm.count("verbose")), minos(vm.count("minos")),
        fixBao(vm.count("fix-bao")), noBBand(vm.count("no-bband"));

    // Check for the required filename parameters.
    if(0 == dataName.length()) {
        std::cerr << "Missing required parameter --data." << std::endl;
        return -1;
    }
    if(0 == fiducialName.length()) {
        std::cerr << "Missing required parameter --fiducial." << std::endl;
        return -1;
    }
    if(0 == nowigglesName.length()) {
        std::cerr << "Missing required parameter --nowiggles." << std::endl;
        return -1;
    }

    // Initialize the cosmology calculations we will need.
    cosmo::AbsHomogeneousUniversePtr cosmology;
    LyaBaoModelPtr model;
    try {
        // Build the homogeneous cosmology we will use.
        if(OmegaMatter == 0) OmegaMatter = 1 - OmegaLambda;
        cosmology.reset(new cosmo::LambdaCdmUniverse(OmegaLambda,OmegaMatter));
        
         // Build our fit model from tabulated ell=0,2,4 correlation functions on disk.
         model.reset(new LyaBaoModel(fiducialName,nowigglesName,zref));

        if(verbose) std::cout << "Cosmology initialized." << std::endl;
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR during cosmology initialization:\n  " << e.what() << std::endl;
        return -2;
    }
    catch(lk::RuntimeError const &e) {
        std::cerr << "ERROR during cosmology initialization:\n  " << e.what() << std::endl;
        return -2;
    }
    
    // Load the data we will fit.
    LyaDataPtr data;
    try {
        // Initialize the (logLambda,separation,redshift) binning from command-line params.
        AbsBinningPtr llBins,sepBins(new UniformBinning(nsep,minsep,dsep)), zBins(new UniformBinning(nz,minz,dz));
        if(0 == dll2) {
            llBins.reset(new UniformBinning(nll,minll,dll));
        }
        else {
            llBins.reset(new TwoStepBinning(nll,minll,dll,dll2));
        }
        // Initialize the dataset we will fill.
        data.reset(new LyaData(llBins,sepBins,zBins,cosmology));
        // General stuff we will need for reading both files.
        std::string line;
        int lineNumber(0);
        // Capturing regexps for positive integer and signed floating-point constants.
        std::string ipat("(0|(?:[1-9][0-9]*))"),fpat("([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)");
        boost::match_results<std::string::const_iterator> what;
        // Loop over lines in the parameter file.
        std::string paramsName(dataName + ".params");
        std::ifstream paramsIn(paramsName.c_str());
        if(!paramsIn.good()) throw cosmo::RuntimeError("Unable to open " + paramsName);
        boost::regex paramPattern(
            boost::str(boost::format("\\s*%s\\s+%s\\s*\\| Lya covariance 3D \\(%s,%s,%s\\)\\s*")
            % fpat % fpat % fpat % fpat % fpat));
        while(paramsIn.good() && !paramsIn.eof()) {
            std::getline(paramsIn,line);
            if(paramsIn.eof()) break;
            if(!paramsIn.good()) {
                throw cosmo::RuntimeError("Unable to read line " + boost::lexical_cast<std::string>(lineNumber));
            }
            lineNumber++;
            // Parse this line with a regexp.
            if(!boost::regex_match(line,what,paramPattern)) {
                throw cosmo::RuntimeError("Badly formatted params line " +
                    boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
            }
            int nTokens(5);
            std::vector<double> token(nTokens);
            for(int tok = 0; tok < nTokens; ++tok) {
                token[tok] = boost::lexical_cast<double>(std::string(what[tok+1].first,what[tok+1].second));
            }
            // Add this bin to our dataset. Second value token[1] might be non-zero, in which case it is
            // Cinv*d from the quadratic estimator, but we just ignore it.
            data->addData(token[0],token[2],token[3],token[4]);
        }
        data->finalizeData();
        paramsIn.close();
        if(verbose) {
            std::cout << "Read " << data->getNData() << " of " << data->getSize()
                << " data values from " << paramsName << std::endl;
        }
        // Loop over lines in the covariance file.
        std::string covName(dataName + ".cov");
        std::ifstream covIn(covName.c_str());
        if(!covIn.good()) throw cosmo::RuntimeError("Unable to open " + covName);
        boost::regex covPattern(boost::str(boost::format("\\s*%s\\s+%s\\s+%s\\s*") % ipat % ipat % fpat));
        lineNumber = 0;
        while(covIn.good() && !covIn.eof()) {
            std::getline(covIn,line);
            if(covIn.eof()) break;
            if(!covIn.good()) {
                throw cosmo::RuntimeError("Unable to read line " + boost::lexical_cast<std::string>(lineNumber));
            }
            lineNumber++;
            // Parse this line with a regexp.
            if(!boost::regex_match(line,what,covPattern)) {
                throw cosmo::RuntimeError("Badly formatted cov line " +
                    boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
            }
            int index1(boost::lexical_cast<int>(std::string(what[1].first,what[1].second)));
            int index2(boost::lexical_cast<int>(std::string(what[2].first,what[2].second)));
            double value(boost::lexical_cast<double>(std::string(what[3].first,what[3].second)));
            // Add this covariance to our dataset.
            data->addCovariance(index1,index2,value);
        }
        data->finalizeCovariance();
        covIn.close();
        if(verbose) {
            int ndata = data->getNData();
            int ncov = (ndata*(ndata+1))/2;
            std::cout << "Read " << data->getNCov() << " of " << ncov
                << " covariance values from " << covName << std::endl;
        }
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR while reading data:\n  " << e.what() << std::endl;
        return -2;
    }
    
    // Minimize the -log(Likelihood) function.
    try {
        lk::GradientCalculatorPtr gcptr;
        LyaBaoLikelihood nll(data,model,rmin,rmax,fixBao,noBBand);
        lk::FunctionPtr fptr(new lk::Function(boost::ref(nll)));

        int npar(nll.getNPar());
        lk::AbsEnginePtr engine = lk::getEngine("mn2::vmetric",fptr,gcptr,npar);
        lk::MinuitEngine &minuit = dynamic_cast<lk::MinuitEngine&>(*engine);        
        lk::MinuitEngine::StatePtr initialState(new ROOT::Minuit2::MnUserParameterState());
        nll.initialize(initialState);
        std::cout << *initialState;
        
        ROOT::Minuit2::MnStrategy strategy(1); // lo(0),med(1),hi(2)
        ROOT::Minuit2::MnMigrad fitter((ROOT::Minuit2::FCNBase const&)minuit,*initialState,strategy); 

        int maxfcn = 100*npar*npar;
        double edmtol = 0.1;
        ROOT::Minuit2::FunctionMinimum fmin = fitter(maxfcn,edmtol);
        
        if(minos) {
            ROOT::Minuit2::MnMinos minosError((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            for(int ipar = 0; ipar < npar; ++ipar) {
                std::pair<double,double> error = minosError(ipar,maxfcn);
                std::cout << "MINOS error[" << ipar << "] = +" << error.second
                    << ' ' << error.first << std::endl;
            }
        }
        
        std::cout << fmin;
        std::cout << fmin.UserCovariance();
        std::cout << fmin.UserState().GlobalCC();
        
        std::vector<ContourPoints> contourData;
        if(ncontour > 0) {
            if(verbose) std::cout << "Calculating contours with " << ncontour << " points..." << std::endl;
            // 95% CL (see http://wwwasdoc.web.cern.ch/wwwasdoc/minuit/node33.html)
            // Calculate in mathematica using:
            // Solve[CDF[ChiSquareDistribution[2], x] == 0.95, x]
            nll.setErrorScale(5.99146);
            fmin = fitter(maxfcn,edmtol);
            ROOT::Minuit2::MnContours contours95((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            // Parameter indices: 1=bias, 2=beta, 3=BAO amp, 4=BAO scale, 5=bband a1/10, 6=bband a2/1000
            contourData.push_back(contours95(5,6,ncontour));
            contourData.push_back(contours95(4,6,ncontour));
            contourData.push_back(contours95(1,6,ncontour));
            contourData.push_back(contours95(5,3,ncontour));
            contourData.push_back(contours95(4,3,ncontour));
            contourData.push_back(contours95(1,3,ncontour));            
            contourData.push_back(contours95(5,2,ncontour));
            contourData.push_back(contours95(4,2,ncontour));
            contourData.push_back(contours95(1,2,ncontour));
            // 68% CL
            nll.setErrorScale(2.29575);
            fmin = fitter(maxfcn,edmtol);
            ROOT::Minuit2::MnContours contours68((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            contourData.push_back(contours68(5,6,ncontour));
            contourData.push_back(contours68(4,6,ncontour));
            contourData.push_back(contours68(1,6,ncontour));
            contourData.push_back(contours68(5,3,ncontour));
            contourData.push_back(contours68(4,3,ncontour));
            contourData.push_back(contours68(1,3,ncontour));            
            contourData.push_back(contours68(5,2,ncontour));
            contourData.push_back(contours68(4,2,ncontour));
            contourData.push_back(contours68(1,2,ncontour));
        }
        
        if(dumpName.length() > 0) {
            if(verbose) std::cout << "Dumping fit results to " << dumpName << std::endl;
            nll.dump(dumpName,fmin.UserParameters().Params(),contourData,modelBins);
        }
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR during fit:\n  " << e.what() << std::endl;
        return -2;
    }
    catch(lk::RuntimeError const &e) {
        std::cerr << "ERROR during fit:\n  " << e.what() << std::endl;
        return -2;
    }

    // All done: normal exit.
    return 0;
}