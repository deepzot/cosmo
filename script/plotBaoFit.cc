// Created 7-Feb-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>
// ROOT macro to plot results of the baofit program.

#include <fstream>
#include <iostream>
#include <cassert>

void drawLabel(const char *label) {
    TLatex *latex = new TLatex();
    latex->SetTextAlign(33);
    latex->SetTextFont(31);
    latex->SetTextSize(0.07);
    latex->DrawTextNDC(0.95,0.95,label);
}

void drawMarker(double const *pValues, int px, int py) {
    TMarker *marker = new TMarker(pValues[px],pValues[py],20);
    marker->SetMarkerColor(kBlue-8);
    marker->SetMarkerSize(1);
    marker->Draw();
}

void drawLines(TCanvas *c, int ipad, bool xline, bool yline) {
    c->cd(ipad);
    TVirtualPad *pad = c->GetPad(ipad);
    if(xline) { // vertical line at constant x=1
        double ymin = pad->GetUymin(), ymax = pad->GetUymax();
        TLine *line = new TLine(1,ymin,1,ymax);
        line->SetLineColor(kRed-8);
        line->SetLineWidth(2);
        line->Draw();
    }
    if(yline) { // horizontal line at constant y=1
        double xmin = pad->GetUxmin(), xmax = pad->GetUxmax();
        TLine *line = new TLine(xmin,1,xmax,1);
        line->SetLineColor(kRed-8);
        line->SetLineWidth(2);
        line->Draw();
    }
}

double *readBinning(std::istream &is, int &nbins, double scale = 1) {
    is >> nbins;
    assert(nbins > 0 && nbins < 1000);
    double *binning = new double[nbins+1];
    for(int bin = 0; bin <= nbins; ++bin) {
        is >> binning[bin];
        binning[bin] *= scale;
    }
    return binning;
}

void plotBaoFit(const char *filename = "fit.dat") {
    // Initialize graphics options.
    gROOT->SetStyle("Plain");
    gStyle->SetOptStat(0);
    gStyle->SetOptTitle(0);
    
    Double_t rGradient[3] = { 0.0, 1.0, 1.0 };
    Double_t gGradient[3] = { 0.0, 1.0, 0.0 };
    Double_t bGradient[3] = { 1.0, 1.0, 0.0 };
    Double_t sGradient[3] = { 0.0, 0.5, 1.0 };
    TColor::CreateGradientColorTable(3,sGradient,rGradient,gGradient,bGradient,256);
    //gStyle->SetPalette(1,0);
    
    // Sound horizon in Mpc/h at zdrag = 1020.49, calculated from Eisenstein & Hu 1997 using the command:
    // cosmocalc --omega-lambda 0.734 --omega-matter 0.266 --omega-baryon 0.0449 --hubble-constant 0.710
    Double_t r3dContours[1] = { 108.719 };
    
    std::ifstream in(filename);
    double cLight = 299.792458; // in units of 1e3 km/s
    // Read the binning parameters.
    int nll,nsep,nz;
    double *llbins = readBinning(in,nll,cLight); // convert log(lam2/lam1) into 1e3 km/s
    double *sepbins = readBinning(in,nsep);
    double *zbins = readBinning(in,nz);
    // Read the number of data points, the number of model bins, and the number of points per contour.
    int ndata,modelBins,ncontour;
    in >> ndata >> modelBins >> ncontour;
    // Calculate the model binning limits.
    double sepMin = sepbins[0],sepMax = sepbins[nsep],llMin = llbins[0],llMax = llbins[nll];
    // Read the best-fit parameter values.
    int npar;
    in >> npar;
    double *pValue = new double[npar];
    for(int ipar = 0; ipar < npar; ++ipar) in >> pValue[ipar];
    
    r3dContours[0] *= pValue[4];
    
    // Initialize the drawing canvas.
    TCanvas *canvas = new TCanvas("canvas","canvas",nz*400,800);
    canvas->SetMargin(0.05,0.01,0.05,0.05);
    canvas->Divide(nz,2,0.,0.);

    // Create a 1D redshfit histogram
    TH1F *zHist = new TH1F("zHist","zHist",nz,zbins);

    // Create 2D histograms in (ll,sep) for each redshift.
    for(int iz = 0; iz < nz; ++iz) {
        double z = zHist->GetBinCenter(iz+1);
        TH2F *dataHist = (TH2F *)gDirectory->Get(Form("data%d",iz));
        if(dataHist) continue;
        TH2F *dataHist = new TH2F(Form("data%d",iz),Form("Fit data for z = %.2f",z),nsep,sepbins,nll,llbins);
        dataHist->SetXTitle("Angular separation (arcmin)");
        dataHist->SetYTitle("Relative radial velocity (10^{3}km/s)");
        TH2F *pullHist = dataHist->Clone(Form("pull%d",iz));
        TH2F *modelHist = new TH2F(Form("model%d",iz),Form("Model predictions for z = %.2f",z),
            modelBins,sepMin,sepMax,modelBins,llMin,llMax);
        TH2F *r3dHist = new TH2F(Form("r3d%d",iz),Form("Co-moving 3D separation for z = %.2f",z),
            modelBins,sepMin,sepMax,modelBins,llMin,llMax);
    }
    
    // Loop over bin data in the input file.
    int index;
    double data,pull;
    double *sumSqData = new double[nz];
    for(int iz = 0; iz < nz; ++iz) {
        sumSqData[iz] = 0;
    }
    for(int k = 0; k < ndata; ++k) {
        in >> index >> data >> pull;
        int iz = index % nz;
        sumSqData[iz] += data*data;
        TH2F *dataHist = (TH2F *)gDirectory->Get(Form("data%d",iz));
        TH2F *pullHist = (TH2F *)gDirectory->Get(Form("pull%d",iz));
        int isep = 1 + (index/nz) % nsep;
        int ill = 1 + (index/(nz*nsep)) % nll;
        dataHist->SetBinContent(isep,ill,data);
        pullHist->SetBinContent(isep,ill,pull);
    }
    for(int iz = 0; iz < nz; ++iz) {
        std::cout << "RMS = " << std::sqrt(sumSqData[iz]) << std::endl;
    }
    
    // Loop over model predictions in the input file.
    double r3d,pred;
    for(int iz = 0; iz < nz; ++iz) {
        TH2F *modelHist = (TH2F *)gDirectory->Get(Form("model%d",iz));
        TH2F *r3dHist = (TH2F *)gDirectory->Get(Form("r3d%d",iz));
        for(int isep = 0; isep < modelBins; ++isep) {
            for(int ill = 0; ill < modelBins; ++ill) {
                in >> r3d >> pred;
                r3dHist->SetBinContent(isep+1,ill+1,r3d);
                modelHist->SetBinContent(isep+1,ill+1,pred);
            }
        }
    }

    // Draw plots.
    double nsig=3;
    for(iz = 0; iz < nz; ++iz) {
        double zval = zHist->GetBinCenter(iz+1);
        double dataPlotLimit = 0.01*std::sqrt(sumSqData[iz]);
        TH2F *dataHist = (TH2F *)gDirectory->Get(Form("data%d",iz));
        TH2F *pullHist = (TH2F *)gDirectory->Get(Form("pull%d",iz));
        TH2F *modelHist = (TH2F *)gDirectory->Get(Form("model%d",iz));
        TH2F *r3dHist = (TH2F *)gDirectory->Get(Form("r3d%d",iz));

        canvas->cd(iz+1);
        canvas->GetPad(iz+1)->SetMargin(0.11,0.03,0.10,0.01);
        dataHist->GetYaxis()->SetTitleOffset(1.3);
        dataHist->SetMaximum(+dataPlotLimit);
        dataHist->SetMinimum(-dataPlotLimit*0.99); // factor of 0.99 ensures that zero is white
        // Trunctate bins outside the limits so that they are colored correctly.
        for(int ill = 0; ill < nll; ++ill) {
            for(int isep = 0; isep < nsep; ++isep) {
                double data = dataHist->GetBinContent(isep+1,ill+1);
                if(data < -dataPlotLimit) data = -0.98*dataPlotLimit;
                if(data > +dataPlotLimit) data = +0.98*dataPlotLimit;
                dataHist->SetBinContent(isep+1,ill+1,data);
                double pull = pullHist->GetBinContent(isep+1,ill+1);
                if(pull < -nsig) pull = -0.98*nsig;
                if(pull > +nsig) pull = +0.98*nsig;
                pullHist->SetBinContent(isep+1,ill+1,pull);
            }
        }
        dataHist->Draw("col");
        modelHist->SetMaximum(+dataPlotLimit);
        modelHist->SetMinimum(-dataPlotLimit);
        modelHist->Draw("cont3same");
        r3dHist->SetContour(1,r3dContours);
        r3dHist->SetLineWidth(5);
        r3dHist->SetLineColor(kGreen-6);
        //r3dHist->Draw("cont3same");
        drawLabel(Form("z = %.1f",zval));

        canvas->cd(nz+iz+1);
        canvas->GetPad(nz+iz+1)->SetMargin(0.11,0.03,0.10,0.01);
        pullHist->GetYaxis()->SetTitleOffset(1.3);
        pullHist->SetMaximum(+nsig);
        pullHist->SetMinimum(-nsig*0.99); // factor of 0.99 ensures that zero is white
        pullHist->Draw("col");
        drawLabel(Form("z = %.1f",zval));
    }

    if(ncontour > 0) {
        TCanvas *canvas2 = new TCanvas("canvas2","canvas2",1000,1000);
        canvas2->Divide(3,3,0.001,0.001);
        Double_t *xContour = new Double_t[ncontour+1], *yContour = new Double_t[ncontour+1];
        int ngraph = 9;
        TGraph **contourGraph = new TGraph*[2*ngraph];
        for(int ig = 0; ig < 2*ngraph; ++ig) {
            for(int i = 0; i < ncontour; ++i) in >> xContour[i] >> yContour[i];
            // Close the contour
            xContour[ncontour] = xContour[0];
            yContour[ncontour] = yContour[0];
            contourGraph[ig] = new TGraph(ncontour+1,xContour,yContour);
            int ipad = ig%ngraph+1;
            canvas2->cd(ipad);
            canvas2->GetPad(ipad)->SetMargin(0.15,0.02,0.10,0.02);
            canvas2->GetPad(ipad)->SetGridx();
            canvas2->GetPad(ipad)->SetGridy();
            contourGraph[ig]->SetLineColor(kBlue-8);
            contourGraph[ig]->SetLineWidth(3);
            contourGraph[ig]->Draw(ig < ngraph ? "ALW":"L");
        }
        
        contourGraph[0]->GetHistogram()->SetXTitle("Broadband Power a_{1}/10");
        contourGraph[0]->GetHistogram()->SetYTitle("Broadband Power a_{2}/10^{3}");
        contourGraph[0]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[1]->GetHistogram()->SetXTitle("BAO Relative Scale");
        contourGraph[1]->GetHistogram()->SetYTitle("Broadband Power a_{2}/10^{3}");
        contourGraph[1]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[2]->GetHistogram()->SetXTitle("Lyman-#alpha Tracer Bias");
        contourGraph[2]->GetHistogram()->SetYTitle("Broadband Power a_{2}/10^{3}");
        contourGraph[2]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);

        contourGraph[3]->GetHistogram()->SetXTitle("Broadband Power a_{1}/10");
        contourGraph[3]->GetHistogram()->SetYTitle("BAO Relative Amplitude");
        contourGraph[3]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[4]->GetHistogram()->SetXTitle("BAO Relative Scale");
        contourGraph[4]->GetHistogram()->SetYTitle("BAO Relative Amplitude");
        contourGraph[4]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[5]->GetHistogram()->SetXTitle("Lyman-#alpha Tracer Bias");
        contourGraph[5]->GetHistogram()->SetYTitle("BAO Relative Amplitude");
        contourGraph[5]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);

        contourGraph[6]->GetHistogram()->SetXTitle("Broadband Power a_{1}/10");
        contourGraph[6]->GetHistogram()->SetYTitle("Lyman-#alpha Redshift Distortion #beta");
        contourGraph[6]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[7]->GetHistogram()->SetXTitle("BAO Relative Scale");
        contourGraph[7]->GetHistogram()->SetYTitle("Lyman-#alpha Redshift Distortion #beta");
        contourGraph[7]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        contourGraph[8]->GetHistogram()->SetXTitle("Lyman-#alpha Tracer Bias");
        contourGraph[8]->GetHistogram()->SetYTitle("Lyman-#alpha Redshift Distortion #beta");
        contourGraph[8]->GetHistogram()->GetYaxis()->SetTitleOffset(1.5);
        
        canvas2->Update();
        drawLines(canvas2,1,false,false); drawMarker(pValue,5,6);
        drawLines(canvas2,2,true,false);  drawMarker(pValue,4,6);
        drawLines(canvas2,3,false,false); drawMarker(pValue,1,6);
        drawLines(canvas2,4,false,true);  drawMarker(pValue,5,3);
        drawLines(canvas2,5,true,true);   drawMarker(pValue,4,3);
        drawLines(canvas2,6,false,true);  drawMarker(pValue,1,3);
        drawLines(canvas2,7,false,false); drawMarker(pValue,5,2);
        drawLines(canvas2,8,true,false);  drawMarker(pValue,4,2);
        drawLines(canvas2,9,false,false); drawMarker(pValue,1,2);
    }

    in.close();
}
