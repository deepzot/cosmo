// Created 08-Aug-2011 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

// Reproduce bottom-left plot of Fig.3 in astro-ph/9709112 using:
// cosmocalc --omega-matter 0.2 --omega-baryon 0.1 --hubble-constant 0.5 --cmb-temp 2.728 \
//   --kmin 0.001 --kmax 1 --nk 500 --save-transfer fig3.dat

// Reproduce Fig.1 of JMLG paper draft (needs an extra factor of pi/2 ??)
// ./cosmocalc --omega-baryon 0.044 --omega-matter 0.27 --omega-lambda 0.73 \
//   --hubble-constant 0.71 --save-transfer xfer.dat -r 0.1 --kmax 1

#include "cosmo/cosmo.h"

#include "boost/program_options.hpp"
#include "boost/bind.hpp"
#include "boost/ref.hpp"
#include "boost/lambda/lambda.hpp"

#include <fstream>
#include <iostream>
#include <cmath>

namespace po = boost::program_options;
namespace k = boost::lambda;

class BaoFitPower {
public:
    BaoFitPower(double amplitude, double scale, double sigma,
        cosmo::PowerSpectrumPtr full, cosmo::PowerSpectrumPtr nowiggles)
    : _amplitude(amplitude), _scale(scale), _scale4(scale*scale*scale*scale), _sigsq(sigma*sigma),
        _full(full), _nowiggles(nowiggles)
    { }
    double operator()(double k) const {
        double ak(k/_scale), smooth(std::exp(-ak*ak*_sigsq/2));
        double fullPower = (*_full)(ak), nowigglesPower = (*_nowiggles)(ak);
        return _scale4*(_amplitude*smooth*(fullPower - nowigglesPower) + nowigglesPower);
    }
private:
    double _amplitude, _scale, _scale4, _sigsq;
    cosmo::PowerSpectrumPtr _full, _nowiggles;
}; // BaoFitPower

cosmo::PowerSpectrumPtr createBroadbandPower(double a0, double a1, double a2, double a3) {
    // k::_1 represents k in Mpc/h below.
    cosmo::PowerSpectrumPtr ptr(new cosmo::PowerSpectrum((a0 + (a1 + (a2 + a3/k::_1)/k::_1)/k::_1)/(k::_1*k::_1)));
    return ptr;
}

int main(int argc, char **argv) {
    
    // Configure command-line option processing
    po::options_description cli("Cosmology calculator");
    double OmegaLambda,OmegaMatter,OmegaBaryon,hubbleConstant,cmbTemp,spectralIndex,sigma8,
        zval,kval,kmin,kmax,rval,rmin,rmax,baoAmplitude,baoSigma,baoScale;
    double bbandA0,bbandA1,bbandA2,bbandA3;
    int nk,nr;
    std::string saveTransferFile,saveCorrelationFile;
    cli.add_options()
        ("help,h", "Prints this info and exits.")
        ("verbose", "Prints additional information.")
        ("omega-lambda", po::value<double>(&OmegaLambda)->default_value(0.728),
            "Present-day value of OmegaLambda.")
        ("omega-matter", po::value<double>(&OmegaMatter)->default_value(0),
            "Present-day value of OmegaMatter or zero for 1-OmegaLambda.")
        ("omega-baryon", po::value<double>(&OmegaBaryon)->default_value(0.0456),
            "Present-day value of OmegaBaryon, must be <= OmegaMatter.")
        ("hubble-constant", po::value<double>(&hubbleConstant)->default_value(0.704),
            "Present-day value of the Hubble parameter h = H0/(100 km/s/Mpc).")
        ("cmb-temp", po::value<double>(&cmbTemp)->default_value(2.725),
            "Present-day temperature of the cosmic microwave background in Kelvin.")
        ("spectral-index", po::value<double>(&spectralIndex)->default_value(1),
            "Power exponent of primordial fluctuations.")
        ("sigma8", po::value<double>(&sigma8)->default_value(0),
            "Power will be normalized to this value (default is COBE normalization).")
        ("redshift,z", po::value<double>(&zval)->default_value(1),
            "Emitter redshift.")
        ("wavenumber,k", po::value<double>(&kval)->default_value(0.1),
            "Perturbation wavenumber in 1/(Mpc/h).")
        ("radius,r", po::value<double>(&rval)->default_value(0.04),
            "Radius for 1D power spectrum in Mpc/h.")
        ("save-transfer", po::value<std::string>(&saveTransferFile)->default_value(""),
            "Saves the matter transfer function to the specified filename.")
        ("kmin", po::value<double>(&kmin)->default_value(0.001),
            "Minimum wavenumber in 1/(Mpc/h) for tabulating transfer function.")
        ("kmax", po::value<double>(&kmax)->default_value(100.),
            "Maximum wavenumber in 1/(Mpc/h) for tabulating transfer function.")
        ("nk", po::value<int>(&nk)->default_value(100),
            "Number of logarithmic steps to use for tabulating transfer function.")
        ("save-correlation", po::value<std::string>(&saveCorrelationFile)->default_value(""),
            "Saves the matter correlation function to the specified filename.")
        ("rmin", po::value<double>(&rmin)->default_value(0.01),
            "Minimum radius in (Mpc/h) for tabulating correlation function.")
        ("rmax", po::value<double>(&rmax)->default_value(1000.),
            "Maximum radius in (Mpc/h) for tabulating correlation function.")
        ("nr", po::value<int>(&nr)->default_value(100),
            "Number of logarithmic steps to use for tabulating correlation function.")
        ("rlog", "Use log spaced r-values for saved correlation function (default is linear).")
        ("quad", "Calculates the quadrupole (l=2) correlation function (default is monopole).")
        ("hexa", "Calculates the hexedacapole (l=4) correlation function (default is monopole).")
        ("no-wiggles", "Calculates the power spectrum without baryon acoustic oscillations.")
        ("periodic-wiggles", "Calculates the power spectrum with periodic acoustic oscillations.")
        ("bao-fit", "Calculates a power spectrum parameterized for BAO fitting.")
        ("bao-amplitude", po::value<double>(&baoAmplitude)->default_value(1),
            "Amplitude of baryon acoustic oscillations relative to fiducial model.")
        ("bao-sigma", po::value<double>(&baoSigma)->default_value(0),
            "Gaussian smearing of BAO correlation function peak in Mpc/h relative to fiducial model.")
        ("bao-scale", po::value<double>(&baoScale)->default_value(1),
            "Rescaling of wavenumber relative to fiducial model (>1 means larger acoustic scale)")
        ("broadband-only", "Calculates contribution of broadband power only.")
        ("broadband-a0", po::value<double>(&bbandA0)->default_value(0),
            "Coefficient of 1/k^2 in broadband power model.")
        ("broadband-a1", po::value<double>(&bbandA1)->default_value(0),
            "Coefficient of 1/k^3 in broadband power model.")
        ("broadband-a2", po::value<double>(&bbandA2)->default_value(0),
            "Coefficient of 1/k^4 in broadband power model.")
        ("broadband-a3", po::value<double>(&bbandA3)->default_value(0),
            "Coefficient of 1/k^5 in broadband power model.")
        ;

    // do the command line parsing now
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
    bool verbose(vm.count("verbose")), rlog(vm.count("rlog")),
        quad(vm.count("quad")), hexa(vm.count("hexa")), noWiggles(vm.count("no-wiggles")),
        periodicWiggles(vm.count("periodic-wiggles")), baoFit(vm.count("bao-fit")),
        bbandOnly(vm.count("broadband-only"));

    // Process the multipole flags.
    if(quad && hexa) {
        std::cerr << "Cannot request both quad (l=2) and hexa (l=4) for correlation function output."
            << std::endl;
        return -1;
    }
    cosmo::PowerSpectrumCorrelationFunction::Multipole
        multipole(cosmo::PowerSpectrumCorrelationFunction::Monopole);
    if(quad) multipole = cosmo::PowerSpectrumCorrelationFunction::Quadrupole;
    if(hexa) multipole = cosmo::PowerSpectrumCorrelationFunction::Hexadecapole;

    // Process the wiggle flags.
    if(vm.count("no-wiggles")+vm.count("periodic-wiggles")+vm.count("bao-fit") > 1) {
        std::cerr << "Specify at most one of no-wiggles, periodic-wiggles, bao-fit options."
            << std::endl;
        return -1;
    }
    cosmo::BaryonPerturbations::BaoOption baoOption(cosmo::BaryonPerturbations::ShiftedOscillation);
    if(noWiggles) baoOption = cosmo::BaryonPerturbations::NoOscillation;
    if(periodicWiggles) baoOption = cosmo::BaryonPerturbations::PeriodicOscillation;

    // Build the homogeneous cosmology we will use.
    if(OmegaMatter == 0) OmegaMatter = 1 - OmegaLambda;
    cosmo::AbsHomogeneousUniversePtr cosmology(
        new cosmo::LambdaCdmUniverse(OmegaLambda,OmegaMatter));
    std::cout << "curvature = " << cosmology->getCurvature() << std::endl;
    
    std::cout << "z = " << zval << std::endl;
    std::cout << "D(z) = " << cosmology->getLineOfSightComovingDistance(zval) << " Mpc/h"
        << std::endl;
    std::cout << "DM(z) = " << cosmology->getTransverseComovingScale(zval) << " Mpc/h/rad"
        << std::endl;
    double tL(cosmology->getLookbackTime(zval));
    double conv(1e9*86400*365.25);
    std::cout << "t(lookback,z) = " << tL << " secs/h = " << tL/conv*hubbleConstant
        << " Gyr" << std::endl;
    std::cout << "D1(z) = " << 2.5*OmegaMatter*cosmology->getGrowthFunction(zval)
        << std::endl;
    
    cosmo::BaryonPerturbations baryons(OmegaMatter,OmegaBaryon,hubbleConstant,cmbTemp,baoOption);

    std::cout << "z(eq) = " << baryons.getMatterRadiationEqualityRedshift() << std::endl;
    std::cout << "k(eq) = " << baryons.getMatterRadiationEqualityScale() << " /(Mpc/h)"
        << std::endl;
    std::cout << "sound horizon = " << baryons.getSoundHorizon() << " Mpc/h at z(drag) = "
        << baryons.getDragEpoch() << std::endl;
    std::cout << "Silk damping scale = " << baryons.getSilkDampingScale() << " /(Mpc/h)"
        << std::endl;

    double Tfc,Tfb,Tf;
    baryons.calculateTransferFunctions(kval,Tfc,Tfb,Tf);
    std::cout << "k = " << kval << " /(Mpc/h)" << std::endl;
    std::cout << "Tf(cmb,k) = " << Tfc << std::endl;
    std::cout << "Tf(baryon,k) = " << Tfb << std::endl;
    std::cout << "Tf(full,k) = " << Tf << std::endl;

    // Create a sharable pointer to the matter transfer function.
    cosmo::TransferFunctionPtr transferPtr(new cosmo::TransferFunction(boost::bind(
        &cosmo::BaryonPerturbations::getMatterTransfer,&baryons,_1)));

    // Use COBE normalization for n=1
    double deltaH(1.94e-5*std::pow(OmegaMatter,-0.785-0.05*std::log(OmegaMatter)));
    std::cout << "deltaH = " << deltaH << std::endl;

    cosmo::TransferFunctionPowerSpectrum transferPower(transferPtr,spectralIndex,deltaH);
    cosmo::PowerSpectrumPtr power(new cosmo::PowerSpectrum(boost::ref(transferPower)));

    // Calculate the Gaussian RMS amplitude on the Jean's length appropriate for
    // QSO spectra, evolved for z = 3.
    double rQSO(0.0416/std::sqrt(OmegaMatter)); // in Mpc/h
    double evol(cosmology->getGrowthFunction(3)/cosmology->getGrowthFunction(0));
    double sigmaQSO(cosmo::getRmsAmplitude(power,rQSO,true));
    std::cout << "rQSO = " << rQSO << " Mpc/h, sigmaQSO(z=0) = " << sigmaQSO
        << ", sigmaQSO(z=3) = " << sigmaQSO*evol << std::endl;

    // Check the normalization at 8 Mpc/h scales.
    double sig8pred(0.5*std::pow(OmegaMatter,-0.65));
    double sig8calc(cosmo::getRmsAmplitude(power,8));
    std::cout << "sigma(8 Mpc/h) = " << sig8calc
        << " (pred = " << sig8pred << ")" << std::endl;
    double norm(1);
    if(sigma8 > 0) {
        std::cout << "Rescaling to sigma(8 Mpc/h) = " << sigma8 << std::endl;
        double tmp(sigma8/sig8calc);
        norm = tmp*tmp;
    }
    
    // Calculate the growth factor from zval to z=0
    evol = cosmology->getGrowthFunction(zval)/cosmology->getGrowthFunction(0);
    double evolSq(evol*evol);

    // Add BAO fitting parameters if requested
    boost::shared_ptr<cosmo::BaryonPerturbations> noWigglesBaryonsPtr;
    cosmo::TransferFunctionPtr noWigglesTransferPtr;
    boost::shared_ptr<cosmo::TransferFunctionPowerSpectrum> noWigglesTransferPowerPtr;
    cosmo::PowerSpectrumPtr noWigglesPowerPtr;
    boost::shared_ptr<BaoFitPower> baoFitPowerPtr;
    if(baoFit) {
        noWigglesBaryonsPtr.reset(new cosmo::BaryonPerturbations(
            OmegaMatter,OmegaBaryon,hubbleConstant,cmbTemp,cosmo::BaryonPerturbations::NoOscillation));
        noWigglesTransferPtr.reset(new cosmo::TransferFunction(boost::bind(
            &cosmo::BaryonPerturbations::getMatterTransfer,noWigglesBaryonsPtr,_1)));
        noWigglesTransferPowerPtr.reset(new cosmo::TransferFunctionPowerSpectrum(
            noWigglesTransferPtr,spectralIndex,deltaH));
        noWigglesPowerPtr.reset(new cosmo::PowerSpectrum(boost::ref(*noWigglesTransferPowerPtr)));
        baoFitPowerPtr.reset(new BaoFitPower(baoAmplitude,baoScale,baoSigma,power,noWigglesPowerPtr));
        power.reset(new cosmo::PowerSpectrum(boost::ref(*baoFitPowerPtr)));
    }
    
    // Create broadband power model, if requested.
    if(0 != bbandA0 || 0 != bbandA1 || 0 != bbandA2 || 0 != bbandA3) {
        cosmo::PowerSpectrumPtr broadband = createBroadbandPower(bbandA0,bbandA1,bbandA2,bbandA3);     
    }
    else if(bbandOnly) {
        std::cerr << "Must have at least one non-zero broadband coefficient for broadband-only." << std::endl;
        return -2;
    }

    if(0 < saveTransferFile.length()) {
        double pi(4*std::atan(1)),fourpi2(4*pi*pi);
        cosmo::OneDimensionalPowerSpectrum onedZero(power,0,kmin,kmax,nk),
            onedHard(power,+rval,kmin,kmax,nk),onedSoft(power,-rval,kmin,kmax,nk);
        std::ofstream out(saveTransferFile.c_str());
        double kratio(std::pow(kmax/kmin,1/(nk-1.)));
        for(int i = 0; i < nk; ++i) {
            double k(kmin*std::pow(kratio,i));
            if(k > kmax) k = kmax; // might happen with rounding
            out << k << ' ' << (*transferPtr)(k) << ' '
                << fourpi2/(k*k*k)*transferPower(k)*evolSq*norm << ' ' << pi/k*onedZero(k)*evolSq*norm
                << ' ' << pi/k*onedHard(k)*evolSq*norm << ' ' << pi/k*onedSoft(k)*evolSq*norm << std::endl;
        }
        out.close();
    }
    
    if(0 < saveCorrelationFile.length()) {
        cosmo::PowerSpectrumCorrelationFunction xi(power,rmin,rmax,multipole,nr);
        std::ofstream out(saveCorrelationFile.c_str());
        double r,dr;
        dr = rlog ? std::pow(rmax/rmin,1/(nr-1.)) : (rmax-rmin)/(nr-1.);
        for(int i = 0; i < nr; ++i) {
            r = rlog ? rmin*std::pow(dr,i) : rmin + dr*i;
            if(r > rmax) r = rmax; // might happen with rounding but xi(r) will complain
            out << r << ' ' << xi(r)*evolSq*norm << std::endl;
        }
        out.close();        
    }

    return 0;
}